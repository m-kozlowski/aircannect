#include "report_signal_store_service.h"

#include <algorithm>
#include <utility>

#include "little_endian.h"
#include "large_object.h"
#include "report_build_checkpoint.h"
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
    release_write_handles();
}

void ReportSignalStoreService::release_write_handles() {
    if (range_write_port_) range_write_port_->release_handles();
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
    std::shared_ptr<LargeByteBuffer> raw,
    bool existing_block,
    bool existing_file,
    uint32_t operation_generation,
    StorageAtomicWriteLane lane,
    bool finalize_header) {
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;

    if (existing_block && (!read_port_ || !existing_file)) {
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_signal_store_block_invalid");
        return OperationAdmission::Rejected;
    }

    const std::shared_ptr<LargeByteBuffer> raw_blocks[] = {raw};
    const OperationAdmission admission = start_blocks(
        track, slot, raw_blocks, 1, existing_file, operation_generation,
        lane, finalize_header);
    if (admission != OperationAdmission::Accepted) return admission;

    raw_ = reinterpret_cast<int16_t *>(raw->data());
    if (existing_block) phase_ = Phase::SubmitRead;
    return OperationAdmission::Accepted;
}

OperationAdmission ReportSignalStoreService::start_blocks(
    const ReportSignalStoreTrack &track,
    size_t first_slot,
    const std::shared_ptr<LargeByteBuffer> *raw_blocks,
    size_t block_count,
    bool existing_file,
    uint32_t operation_generation,
    StorageAtomicWriteLane lane,
    bool finalize_header) {
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;

    if (!range_write_port_ || !raw_blocks || block_count == 0 ||
        block_count > MaxWriteBatchBlocks ||
        (lane != StorageAtomicWriteLane::Foreground &&
         lane != StorageAtomicWriteLane::Maintenance) ||
        operation_generation == 0 || !report_signal_store_track_valid(track) ||
        first_slot >= track.block_slot_count ||
        block_count > track.block_slot_count - first_slot) {
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_signal_store_blocks_invalid");
        return OperationAdmission::Rejected;
    }

    for (size_t i = 0; i < block_count; ++i) {
        if (!raw_blocks[i] || raw_blocks[i]->size() !=
                (REPORT_SIGNAL_STORE_BLOCK_MS / track.sample_interval_ms) * 2 ||
            !(track.present_blocks[(first_slot + i) / 8] &
              (1u << ((first_slot + i) % 8)))) {
            copy_cstr(status_.error, sizeof(status_.error),
                      "report_signal_store_blocks_invalid");
            return OperationAdmission::Rejected;
        }
    }

    const int64_t block_start = track.first_block_start_ms +
        static_cast<int64_t>(first_slot) * REPORT_SIGNAL_STORE_BLOCK_MS;
    ReportSignalStorePlaneRange range;
    if (!ReportSignalStoreFileCodec::plane_range(
            track, block_start, block_count,
            ReportSignalStoreLevel::Raw, range) ||
        range.length == 0 || range.length > AC_STORAGE_RANGE_WRITE_MAX_BYTES ||
        range.length % sizeof(int16_t) != 0) {
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_signal_store_blocks_range_invalid");
        return OperationAdmission::Rejected;
    }

    track_ = track;
    slot_ = first_slot;
    raw_ = nullptr;
    for (size_t i = 0; i < MaxWriteBatchBlocks; ++i) {
        raw_buffers_[i] = i < block_count ? raw_blocks[i] : nullptr;
        raw_blocks_[i] = raw_buffers_[i]
            ? reinterpret_cast<const int16_t *>(raw_buffers_[i]->data()) : nullptr;
    }
    block_count_ = block_count;
    existing_file_ = existing_file;
    write_header_ = !existing_file || finalize_header;
    range_ = range;
    level_ = ReportSignalStoreLevel::Raw;
    operation_generation_ = operation_generation;
    lane_ = lane;
    status_ = {};
    status_.state = ReportSignalStoreState::WritingBlock;
    status_.sleep_day = track.sleep_day;
    status_.signal_count = 1;
    phase_ = write_header_ ? Phase::EncodeHeader : Phase::EncodeBlock;
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
    phase_ = range_write_port_ ? Phase::SubmitFinish : Phase::SubmitEvents;
    status_.state = ReportSignalStoreState::PublishingEvents;
    return OperationAdmission::Accepted;
}

ReportSignalStoreLodBatch ReportSignalStoreService::take_lod() {
    if (phase_ != Phase::Ready) return {};
    return std::exchange(deferred_lod_, {});
}

OperationAdmission ReportSignalStoreService::start_lod(
    const ReportSignalStoreTrack &track,
    ReportSignalStoreLevel level,
    size_t first_slot,
    size_t block_count,
    std::shared_ptr<const LargeByteBuffer> bytes,
    bool existing_file,
    uint32_t operation_generation,
    StorageAtomicWriteLane lane,
    bool finalize_header) {
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;

    ReportSignalStorePlaneRange range;
    if (!range_write_port_ || level == ReportSignalStoreLevel::Raw || !bytes ||
        operation_generation == 0 ||
        first_slot >= track.block_slot_count ||
        (lane != StorageAtomicWriteLane::Foreground &&
         lane != StorageAtomicWriteLane::Maintenance) ||
        !ReportSignalStoreFileCodec::plane_range(
            track, track.first_block_start_ms +
                static_cast<int64_t>(first_slot) * REPORT_SIGNAL_STORE_BLOCK_MS,
            block_count, level, range) ||
        !range.length || bytes->size() != range.length ||
        range.length > AC_STORAGE_RANGE_WRITE_MAX_BYTES -
                           ReportSignalStoreFileCodec::HeaderBytes) {
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_signal_store_lod_range_invalid");
        return OperationAdmission::Rejected;
    }

    track_ = track;
    level_ = level;
    slot_ = first_slot;
    block_count_ = block_count;
    range_ = range;
    encoded_lod_ = std::move(bytes);
    existing_file_ = existing_file;
    write_header_ = !existing_file || finalize_header;
    operation_generation_ = operation_generation;
    lane_ = lane;
    status_ = {};
    status_.state = ReportSignalStoreState::WritingBlock;
    status_.sleep_day = track.sleep_day;
    status_.signal_count = 1;
    phase_ = write_header_ ? Phase::EncodeHeader : Phase::EncodeBlock;
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
        phase_ = write_header_ ? Phase::EncodeHeader : Phase::EncodeBlock;
    }
    return true;
}

bool ReportSignalStoreService::encode_current() {
    const int64_t block_start = track_.first_block_start_ms +
        static_cast<int64_t>(slot_) * REPORT_SIGNAL_STORE_BLOCK_MS;

    if (!ReportSignalStoreFileCodec::plane_range(
            track_, block_start, block_count_, level_, range_)) {
        fail("report_signal_store_block_range_invalid");
        return true;
    }

    if (!encoded_lod_ && level_ != ReportSignalStoreLevel::Raw) {
        auto bytes = ReportSignalStoreFileCodec::encode_blocks(
            track_, level_, slot_, raw_blocks_, block_count_, false,
            deferred_lod_.one_second.get());
        if (!bytes) {
            fail("report_signal_store_lod_encode_failed");
            return true;
        }
        if (level_ == ReportSignalStoreLevel::OneSecond) {
            deferred_lod_.one_second = std::move(bytes);
        } else {
            deferred_lod_.ten_seconds = std::move(bytes);
        }
        advance_level();
        return true;
    }

    const bool include_header = phase_ == Phase::EncodeHeader &&
        range_.offset == ReportSignalStoreFileCodec::HeaderBytes;
    const bool separate_header = phase_ == Phase::EncodeHeader && !include_header;
    block_buffers_.reset();
    if (separate_header) {
        block_bytes_ = ReportSignalStoreFileCodec::encode_header(track_, level_);
    } else {
        bool share_bytes = static_cast<bool>(encoded_lod_);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        share_bytes = true;
#endif
        if (share_bytes) {
            block_bytes_ = include_header
                ? ReportSignalStoreFileCodec::encode_header(track_, level_)
                : nullptr;

            auto buffers = std::shared_ptr<StorageWriteBuffers>(
                LargeObject::create<StorageWriteBuffers>(),
                LargeObject::destroy<StorageWriteBuffers>);
            const size_t parts = encoded_lod_ ? 1 : block_count_;
            if (!buffers || !buffers->allocate(parts) ||
                (include_header && !block_bytes_)) {
                fail("report_signal_store_block_allocation_failed");
                return true;
            }
            for (size_t i = 0; i < parts; ++i) {
                *buffers->append() = encoded_lod_ ? encoded_lod_ : raw_buffers_[i];
            }
            block_buffers_ = std::move(buffers);
        } else {
            block_bytes_ = ReportSignalStoreFileCodec::encode_blocks(
                track_, level_, slot_, raw_blocks_, block_count_, include_header);
        }
        if (include_header) {
            range_.offset = 0;
            range_.length += ReportSignalStoreFileCodec::HeaderBytes;
        }
    }

    const size_t expected = separate_header
        ? ReportSignalStoreFileCodec::HeaderBytes : range_.length;

    StorageRangeWriteCommand payload;
    payload.bytes = block_bytes_;
    payload.buffers = block_buffers_;
    write_size_ = payload.size();
    if (write_size_ != expected ||
        expected == 0 || expected > AC_STORAGE_RANGE_WRITE_MAX_BYTES) {
        fail("report_signal_store_block_encode_failed");
        return true;
    }

    phase_ = separate_header
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
    command.buffers = block_buffers_;
    command.offset = header ? 0 : range_.offset;
    command.truncate = command.offset == 0 && !existing_file_;
    command.retain_handle = true;
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
        completion.bytes_written != write_size_) {
        fail(completion.error[0] ? completion.error
                                 : "report_signal_store_range_write_failed");
        return true;
    }

    status_.bytes_written += completion.bytes_written;
    block_bytes_.reset();
    block_buffers_.reset();
    if (phase_ == Phase::WaitHeader) {
        phase_ = Phase::EncodeBlock;
    } else {
        advance_level();
    }
    return true;
}

bool ReportSignalStoreService::finish_writes() {
    if (phase_ == Phase::SubmitFinish) {
        StorageRangeWriteCommand command;
        command.finish = true;
        command.generation = operation_generation_;
        command.lane = lane_;
        const auto submission = range_write_port_->request_write(command);
        if (submission.admission == OperationAdmission::Busy) return false;
        if (!submission.accepted()) {
            fail("report_signal_store_finish_rejected");
            return true;
        }
        range_ticket_ = submission.ticket;
        phase_ = Phase::WaitFinish;
        return true;
    }

    StorageRangeWriteCompletion completion;
    if (!range_write_port_->take_completion(range_ticket_, completion)) return false;
    range_ticket_ = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        fail(completion.error[0] ? completion.error : "report_signal_store_close_failed");
    } else {
        phase_ = Phase::SubmitEvents;
    }
    return true;
}

void ReportSignalStoreService::advance_level() {
    if (!encoded_lod_ && level_ == ReportSignalStoreLevel::Raw &&
        (track_.lod_mask & REPORT_SIGNAL_STORE_LOD_1S)) {
        level_ = ReportSignalStoreLevel::OneSecond;
    } else if (!encoded_lod_ && level_ != ReportSignalStoreLevel::TenSeconds &&
               (track_.lod_mask & REPORT_SIGNAL_STORE_LOD_10S)) {
        level_ = ReportSignalStoreLevel::TenSeconds;
    } else {
        raw_ = nullptr;
        for (size_t i = 0; i < MaxWriteBatchBlocks; ++i) {
            raw_blocks_[i] = nullptr;
            raw_buffers_[i].reset();
        }
        block_count_ = 0;
        status_.signal_index = 1;
        phase_ = Phase::Ready;
        status_.state = ReportSignalStoreState::Ready;
        return;
    }

    phase_ = write_header_ ? Phase::EncodeHeader : Phase::EncodeBlock;
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
        case Phase::SubmitCheckpoint:
        case Phase::WaitCheckpoint:
            return bundle_->checkpoint;
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
        case Phase::SubmitCheckpoint:
        case Phase::WaitCheckpoint:
            return report_build_checkpoint_path(
                bundle_->sleep_day, bundle_->generation,
                bundle_->checkpoint_slot, path, path_size);
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
        case Phase::SubmitCheckpoint:
            phase_ = Phase::WaitCheckpoint;
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
            phase_ = bundle_->checkpoint ? Phase::SubmitCheckpoint
                                         : Phase::SubmitMetadata;
            status_.state = ReportSignalStoreState::PublishingMetadata;
            break;
        case Phase::WaitCheckpoint:
            bundle_->checkpoint.reset();
            phase_ = Phase::SubmitMetadata;
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
        case Phase::SubmitFinish:
        case Phase::WaitFinish:
            return finish_writes();
        case Phase::SubmitEvents:
        case Phase::SubmitCheckpoint:
        case Phase::SubmitMetadata:
            return submit_current();
        case Phase::WaitEvents:
        case Phase::WaitCheckpoint:
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
    block_buffers_.reset();
    encoded_lod_.reset();
    deferred_lod_ = {};
    write_size_ = 0;
    raw_ = nullptr;
    for (size_t i = 0; i < MaxWriteBatchBlocks; ++i) {
        raw_blocks_[i] = nullptr;
        raw_buffers_[i].reset();
    }
    block_count_ = 0;

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
    raw_ = nullptr;
    for (size_t i = 0; i < MaxWriteBatchBlocks; ++i) {
        raw_blocks_[i] = nullptr;
    }
    block_count_ = 0;
    existing_file_ = false;
    write_header_ = true;
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
