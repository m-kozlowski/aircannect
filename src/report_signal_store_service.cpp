#include "report_signal_store_service.h"

#include <algorithm>
#include <utility>

#include "little_endian.h"
#include "string_util.h"

namespace aircannect {
namespace {

bool publishing_state(ReportSignalStoreState state) {
    return state == ReportSignalStoreState::WritingBlock ||
           state == ReportSignalStoreState::PublishingEvents ||
           state == ReportSignalStoreState::PublishingMetadata;
}

}  // namespace

bool ReportSignalStoreStatus::active() const {
    return publishing_state(state);
}

bool ReportSignalStoreStatus::terminal() const {
    return state == ReportSignalStoreState::Ready ||
           state == ReportSignalStoreState::Failed ||
           state == ReportSignalStoreState::Cancelled;
}

ReportSignalStoreService::~ReportSignalStoreService() {
    cancel();
}

void ReportSignalStoreService::begin(
    StorageReadPort &read_port,
    StorageRangeWritePort &range_write_port,
    StorageAtomicWritePort &write_port) {
    reset();
    read_port_ = &read_port;
    range_write_port_ = &range_write_port;
    write_port_ = &write_port;
}

void ReportSignalStoreService::begin(StorageAtomicWritePort &write_port) {
    reset();
    read_port_ = nullptr;
    range_write_port_ = nullptr;
    write_port_ = &write_port;
}

OperationAdmission ReportSignalStoreService::start_block(
    const ReportSignalStoreTrack &track,
    size_t slot,
    int16_t *raw,
    bool existing_block,
    bool existing_file,
    uint32_t operation_generation,
    StorageAtomicWriteLane lane) {
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;

    if (!read_port_ || !range_write_port_ || !raw ||
        (lane != StorageAtomicWriteLane::Foreground &&
         lane != StorageAtomicWriteLane::Maintenance) ||
        operation_generation == 0 || !report_signal_store_track_valid(track) ||
        slot >= track.block_slot_count || (existing_block && !existing_file) ||
        !(track.present_blocks[slot / 8] & (1u << (slot % 8)))) {
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_signal_store_block_invalid");
        return OperationAdmission::Rejected;
    }

    const int64_t block_start = track.first_block_start_ms +
        static_cast<int64_t>(slot) * REPORT_SIGNAL_STORE_BLOCK_MS;
    ReportSignalStorePlaneRange range;

    if (!ReportSignalStoreFileCodec::plane_range(
            track, block_start, 1, ReportSignalStoreLevel::Raw, range) ||
        range.length == 0 ||
        range.length > ReportSignalStoreFileCodec::MaxBlockBytes ||
        range.length % sizeof(int16_t) != 0) {
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_signal_store_block_range_invalid");
        return OperationAdmission::Rejected;
    }

    track_ = track;
    slot_ = slot;
    raw_ = raw;
    existing_file_ = existing_file;
    range_ = range;
    level_ = ReportSignalStoreLevel::Raw;
    operation_generation_ = operation_generation;
    lane_ = lane;
    status_ = {};
    status_.state = ReportSignalStoreState::WritingBlock;
    status_.sleep_day = track.sleep_day;
    status_.signal_count = 1;
    phase_ = existing_block ? Phase::SubmitRead : Phase::EncodeHeader;
    return OperationAdmission::Accepted;
}

OperationAdmission ReportSignalStoreService::start(
    std::shared_ptr<ReportSignalStoreBundle> bundle,
    uint32_t operation_generation,
    StorageAtomicWriteLane lane) {
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;
    if (!write_port_ || !bundle || !bundle->valid() ||
        operation_generation == 0) {
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_signal_store_publish_invalid");
        return OperationAdmission::Rejected;
    }

    bundle_ = std::move(bundle);
    published_metadata_.reset();
    operation_generation_ = operation_generation;
    lane_ = lane;
    status_ = {};
    status_.sleep_day = bundle_->sleep_day;
    status_.signal_count = bundle_->signal_count();
    status_.signal_index = status_.signal_count;
    phase_ = Phase::SubmitEvents;
    status_.state = ReportSignalStoreState::PublishingEvents;
    return OperationAdmission::Accepted;
}

bool ReportSignalStoreService::submit_read() {
    char path[AC_STORAGE_PATH_MAX] = {};

    if (!report_signal_store_signal_path(
            track_, ReportSignalStoreLevel::Raw, path, sizeof(path))) {
        fail("report_signal_store_block_path_invalid");
        return true;
    }

    StorageReadCommand command;
    command.path = path;
    command.offset = range_.offset;
    command.length = range_.length;
    command.generation = operation_generation_;
    command.lane = lane_ == StorageAtomicWriteLane::Foreground
        ? StorageReadLane::Foreground : StorageReadLane::Maintenance;

    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("report_signal_store_read_rejected");
        return true;
    }

    read_ticket_ = submission.ticket;
    phase_ = Phase::WaitRead;
    return true;
}

bool ReportSignalStoreService::finish_read() {
    StorageReadCompletion completion;
    if (!read_port_->take_completion(read_ticket_, completion)) return false;

    read_ticket_ = {};
    prepared_ = completion.prepared;

    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !prepared_.valid() || prepared_.length != range_.length) {
        fail(completion.error[0] ? completion.error
                                 : "report_signal_store_read_failed");
        return true;
    }

    phase_ = Phase::MergeRead;
    return true;
}

bool ReportSignalStoreService::merge_read() {
    uint8_t bytes[512];
    const size_t wanted = std::min(sizeof(bytes), range_.length - read_offset_);
    const PreparedByteRead read = read_port_->read_prepared(
        prepared_, read_offset_, bytes, wanted);

    if (read.state == PreparedByteReadState::Retry) return false;
    if (read.state != PreparedByteReadState::Data ||
        read.bytes == 0 || read.bytes > wanted) {
        fail("report_signal_store_read_incomplete");
        return true;
    }

    // Prepared reads can split a cell between polls. Preserve supplied samples.
    for (size_t i = 0; i < read.bytes; ++i) {
        const size_t position = read_offset_ + i;
        if (position % sizeof(int16_t) == 0) {
            read_low_byte_ = bytes[i];
        } else if (raw_[position / sizeof(int16_t)] ==
                   track_.missing_value) {
            const uint8_t cell[] = {read_low_byte_, bytes[i]};
            raw_[position / sizeof(int16_t)] =
                static_cast<int16_t>(LittleEndian::get_le16(cell));
        }
    }

    read_offset_ += read.bytes;
    if (read_offset_ == range_.length) {
        read_port_->release_prepared(prepared_);
        prepared_ = {};
        phase_ = Phase::EncodeHeader;
    }
    return true;
}

bool ReportSignalStoreService::encode_current() {
    if (phase_ == Phase::EncodeHeader) {
        block_bytes_ = ReportSignalStoreFileCodec::encode_header(track_, level_);
    } else {
        const int64_t block_start = track_.first_block_start_ms +
            static_cast<int64_t>(slot_) * REPORT_SIGNAL_STORE_BLOCK_MS;

        if (!ReportSignalStoreFileCodec::plane_range(
                track_, block_start, 1, level_, range_)) {
            fail("report_signal_store_block_range_invalid");
            return true;
        }

        block_bytes_ = ReportSignalStoreFileCodec::encode_block(
            track_, level_, slot_, raw_);
    }

    const size_t expected = phase_ == Phase::EncodeHeader
        ? ReportSignalStoreFileCodec::HeaderBytes : range_.length;

    if (!block_bytes_ || block_bytes_->size() != expected ||
        expected == 0 || expected > ReportSignalStoreFileCodec::MaxBlockBytes) {
        fail("report_signal_store_block_encode_failed");
        return true;
    }

    phase_ = phase_ == Phase::EncodeHeader
        ? Phase::SubmitHeader : Phase::SubmitBlock;
    return true;
}

bool ReportSignalStoreService::submit_range() {
    char path[AC_STORAGE_PATH_MAX] = {};

    if (!report_signal_store_signal_path(
            track_, level_, path, sizeof(path))) {
        fail("report_signal_store_block_path_invalid");
        return true;
    }

    const bool header = phase_ == Phase::SubmitHeader;
    StorageRangeWriteCommand command;
    command.path = path;
    command.bytes = block_bytes_;
    command.offset = header ? 0 : range_.offset;
    command.truncate = header && !existing_file_;
    command.generation = operation_generation_;
    command.lane = lane_;

    const OperationSubmission submission =
        range_write_port_->request_write(command);

    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("report_signal_store_range_write_rejected");
        return true;
    }

    range_ticket_ = submission.ticket;
    phase_ = header ? Phase::WaitHeader : Phase::WaitBlock;
    return true;
}

bool ReportSignalStoreService::finish_range() {
    StorageRangeWriteCompletion completion;
    if (!range_write_port_->take_completion(range_ticket_, completion)) {
        return false;
    }

    range_ticket_ = {};

    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !block_bytes_ || completion.bytes_written != block_bytes_->size()) {
        fail(completion.error[0] ? completion.error
                                 : "report_signal_store_range_write_failed");
        return true;
    }

    status_.bytes_written += completion.bytes_written;
    block_bytes_.reset();
    if (phase_ == Phase::WaitHeader) {
        phase_ = Phase::EncodeBlock;
    } else {
        advance_level();
    }
    return true;
}

void ReportSignalStoreService::advance_level() {
    if (level_ == ReportSignalStoreLevel::Raw &&
        (track_.lod_mask & REPORT_SIGNAL_STORE_LOD_1S)) {
        level_ = ReportSignalStoreLevel::OneSecond;
    } else if (level_ != ReportSignalStoreLevel::TenSeconds &&
               (track_.lod_mask & REPORT_SIGNAL_STORE_LOD_10S)) {
        level_ = ReportSignalStoreLevel::TenSeconds;
    } else {
        raw_ = nullptr;
        status_.signal_index = 1;
        phase_ = Phase::Ready;
        status_.state = ReportSignalStoreState::Ready;
        return;
    }

    phase_ = Phase::EncodeHeader;
}

std::shared_ptr<const LargeByteBuffer>
ReportSignalStoreService::current_bytes() const {
    if (!bundle_) return {};

    switch (phase_) {
        case Phase::SubmitEvents:
        case Phase::WaitEvents:
            return bundle_->events;
        case Phase::SubmitMetadata:
        case Phase::WaitMetadata:
            return bundle_->metadata;
        default:
            return {};
    }
}

bool ReportSignalStoreService::current_path(
    char *path,
    size_t path_size) const {
    if (!bundle_) return false;

    switch (phase_) {
        case Phase::SubmitEvents:
        case Phase::WaitEvents:
            return report_signal_store_events_path(
                bundle_->sleep_day,
                bundle_->generation,
                path,
                path_size);
        case Phase::SubmitMetadata:
        case Phase::WaitMetadata:
            return report_signal_store_night_path(
                bundle_->sleep_day, path, path_size);
        default:
            return false;
    }
}

bool ReportSignalStoreService::submit_current() {
    char path[AC_STORAGE_PATH_MAX] = {};
    std::shared_ptr<const LargeByteBuffer> bytes = current_bytes();
    if (!write_port_ || !bundle_ || !bytes ||
        !current_path(path, sizeof(path))) {
        fail("report_signal_store_publish_invalid");
        return true;
    }

    StorageAtomicWriteCommand command;
    command.path = path;
    command.bytes = std::move(bytes);
    command.lane = lane_;
    command.generation = operation_generation_;
    const OperationSubmission submission = write_port_->request_write(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("report_signal_store_write_rejected");
        return true;
    }

    write_ticket_ = submission.ticket;
    switch (phase_) {
        case Phase::SubmitEvents:
            phase_ = Phase::WaitEvents;
            break;
        case Phase::SubmitMetadata:
            phase_ = Phase::WaitMetadata;
            break;
        default:
            fail("report_signal_store_publish_phase_invalid");
            break;
    }
    return true;
}

bool ReportSignalStoreService::finish_current() {
    if (!write_port_ || !bundle_ || !write_ticket_.valid()) {
        fail("report_signal_store_publish_invalid");
        return true;
    }

    StorageAtomicWriteCompletion completion;
    if (!write_port_->take_completion(write_ticket_, completion)) return false;
    write_ticket_ = {};

    const std::shared_ptr<const LargeByteBuffer> expected = current_bytes();
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !expected || completion.bytes_written != expected->size()) {
        fail(completion.error[0] ? completion.error
                                 : "report_signal_store_write_failed");
        return true;
    }
    status_.bytes_written += completion.bytes_written;

    switch (phase_) {
        case Phase::WaitEvents:
            bundle_->release_events();
            phase_ = Phase::SubmitMetadata;
            status_.state = ReportSignalStoreState::PublishingMetadata;
            break;
        case Phase::WaitMetadata:
            published_metadata_ = bundle_->metadata;
            phase_ = Phase::Ready;
            status_.state = ReportSignalStoreState::Ready;
            bundle_.reset();
            break;
        default:
            fail("report_signal_store_publish_phase_invalid");
            break;
    }
    return true;
}

bool ReportSignalStoreService::poll() {
    switch (phase_) {
        case Phase::SubmitRead:
            return submit_read();
        case Phase::WaitRead:
            return finish_read();
        case Phase::MergeRead:
            return merge_read();
        case Phase::EncodeHeader:
        case Phase::EncodeBlock:
            return encode_current();
        case Phase::SubmitHeader:
        case Phase::SubmitBlock:
            return submit_range();
        case Phase::WaitHeader:
        case Phase::WaitBlock:
            return finish_range();
        case Phase::SubmitEvents:
        case Phase::SubmitMetadata:
            return submit_current();
        case Phase::WaitEvents:
        case Phase::WaitMetadata:
            return finish_current();
        case Phase::Idle:
        case Phase::Ready:
        case Phase::Failed:
        case Phase::Cancelled:
            return false;
    }
    return false;
}

void ReportSignalStoreService::fail(const char *error) {
    release_io();
    phase_ = Phase::Failed;
    status_.state = ReportSignalStoreState::Failed;
    copy_cstr(status_.error, sizeof(status_.error), error);
    bundle_.reset();
    published_metadata_.reset();
}

void ReportSignalStoreService::cancel() {
    if (phase_ == Phase::Idle || phase_ == Phase::Ready ||
        phase_ == Phase::Failed || phase_ == Phase::Cancelled) {
        return;
    }

    release_io();
    phase_ = Phase::Cancelled;
    status_.state = ReportSignalStoreState::Cancelled;
    bundle_.reset();
    published_metadata_.reset();
}

void ReportSignalStoreService::release_io() {
    if (read_ticket_.valid() && read_port_) {
        (void)read_port_->abandon(read_ticket_);
    }
    read_ticket_ = {};
    if (prepared_.valid() && read_port_) {
        read_port_->release_prepared(prepared_);
    }
    prepared_ = {};

    if (range_ticket_.valid() && range_write_port_) {
        (void)range_write_port_->abandon(range_ticket_);
    }
    range_ticket_ = {};
    // The storage owner retains shared bytes if cancellation is still queued.
    block_bytes_.reset();
    raw_ = nullptr;

    if (write_ticket_.valid() && write_port_) {
        (void)write_port_->abandon(write_ticket_);
    }
    write_ticket_ = {};
}

void ReportSignalStoreService::clear_operation() {
    release_io();
    track_ = {};
    level_ = ReportSignalStoreLevel::Raw;
    range_ = {};
    slot_ = 0;
    existing_file_ = false;
    read_offset_ = 0;
    read_low_byte_ = 0;
    bundle_.reset();
    published_metadata_.reset();
    write_ticket_ = {};
    operation_generation_ = 0;
    lane_ = StorageAtomicWriteLane::Maintenance;
}

void ReportSignalStoreService::reset() {
    cancel();
    clear_operation();
    phase_ = Phase::Idle;
    status_ = {};
}

std::shared_ptr<const LargeByteBuffer>
ReportSignalStoreService::take_published_metadata() {
    if (phase_ != Phase::Ready || !published_metadata_) return {};
    return std::move(published_metadata_);
}

}  // namespace aircannect
