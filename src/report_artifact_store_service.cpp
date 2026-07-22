#include "report_artifact_store_service.h"

#include <utility>

#include "string_util.h"

namespace aircannect {
namespace {

bool publishing_state(ReportArtifactStoreState state) {
    return state == ReportArtifactStoreState::LoadingManifest ||
           state == ReportArtifactStoreState::PublishingResult ||
           state == ReportArtifactStoreState::PublishingOverview ||
           state == ReportArtifactStoreState::PublishingRangeTile ||
           state == ReportArtifactStoreState::PublishingManifest;
}

StorageReadLane read_lane(StorageAtomicWriteLane lane) {
    return lane == StorageAtomicWriteLane::Foreground
        ? StorageReadLane::Foreground
        : StorageReadLane::Maintenance;
}

}  // namespace

bool ReportArtifactStoreStatus::active() const {
    return publishing_state(state);
}

bool ReportArtifactStoreStatus::terminal() const {
    return state == ReportArtifactStoreState::Ready ||
           state == ReportArtifactStoreState::Failed ||
           state == ReportArtifactStoreState::Cancelled;
}

ReportArtifactStoreService::~ReportArtifactStoreService() {
    cancel();
}

void ReportArtifactStoreService::begin(
    StorageReadPort &read_port,
    StorageAtomicWritePort &write_port) {
    read_port_ = &read_port;
    write_port_ = &write_port;
}

OperationAdmission ReportArtifactStoreService::start(
    std::shared_ptr<const ReportArtifactBundle> bundle,
    uint32_t generation,
    StorageAtomicWriteLane lane) {
    if (!read_port_ || !write_port_) return OperationAdmission::Rejected;
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;
    if (!bundle || !bundle->valid() || generation == 0) {
        return OperationAdmission::Rejected;
    }

    bundle_ = std::move(bundle);
    generation_ = generation;
    lane_ = lane;
    status_ = {};
    status_.key = bundle_->key;

    if (bundle_->key.kind == ReportArtifactKind::RangeTile) {
        phase_ = Phase::SubmitManifestRead;
        status_.state = ReportArtifactStoreState::LoadingManifest;
    } else {
        phase_ = Phase::SubmitResult;
        status_.state = ReportArtifactStoreState::PublishingResult;
    }
    return OperationAdmission::Accepted;
}

bool ReportArtifactStoreService::submit_manifest_read() {
    if (!read_port_ || !bundle_) {
        fail("report_artifact_store_not_ready");
        return true;
    }

    char path[AC_STORAGE_PATH_MAX] = {};
    if (!report_artifact_manifest_path(
            bundle_->key.sleep_day, path, sizeof(path))) {
        fail("report_artifact_manifest_path_invalid");
        return true;
    }

    StorageReadCommand command;
    command.path = path;
    command.length = ReportArtifactManifestCodec::MaxBytes;
    command.lane = read_lane(lane_);
    command.generation = generation_;

    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("report_artifact_manifest_read_rejected");
        return true;
    }

    read_ticket_ = submission.ticket;
    phase_ = Phase::WaitManifestRead;
    return true;
}

bool ReportArtifactStoreService::finish_manifest_read() {
    if (!read_port_ || !read_ticket_.valid() || !bundle_) {
        fail("report_artifact_store_not_ready");
        return true;
    }

    StorageReadCompletion completion;
    if (!read_port_->take_completion(read_ticket_, completion)) return false;
    read_ticket_ = {};

    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.prepared.valid() ||
        completion.prepared.length <
            ReportArtifactManifestCodec::HeaderBytes ||
        completion.prepared.length > ReportArtifactManifestCodec::MaxBytes) {
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        fail("report_artifact_manifest_read_failed");
        return true;
    }

    prepared_ = completion.prepared;
    std::unique_ptr<LargeByteBuffer> bytes =
        LargeByteBuffer::allocate(prepared_.length);
    if (!bytes) {
        fail("report_artifact_manifest_allocation_failed");
        return true;
    }

    const size_t copied = read_port_->read_prepared(
        prepared_, 0, bytes->data(), bytes->size());
    read_port_->release_prepared(prepared_);
    prepared_ = {};
    if (copied != bytes->size()) {
        fail("report_artifact_manifest_short_read");
        return true;
    }

    std::shared_ptr<const LargeByteBuffer> current =
        LargeByteBuffer::freeze(std::move(bytes));
    ReportArtifactManifestView manifest;
    if (!current || !ReportArtifactManifestCodec::decode(
                        current->data(), current->size(), manifest) ||
        manifest.key.sleep_day != bundle_->key.sleep_day ||
        manifest.key.source_revision != bundle_->key.source_revision) {
        fail("report_artifact_manifest_stale");
        return true;
    }

    ReportRangeTileArtifact tile;
    tile.start_ms = bundle_->key.range_start_ms;
    tile.end_ms = bundle_->key.range_end_ms;
    tile.size = bundle_->range_tile->size();
    tile.crc32 = bundle_->range_tile_crc32;
    manifest_bytes_ = ReportArtifactManifestCodec::add_tile(manifest, tile);
    if (!manifest_bytes_) {
        fail("report_artifact_manifest_update_failed");
        return true;
    }

    phase_ = Phase::SubmitRangeTile;
    status_.state = ReportArtifactStoreState::PublishingRangeTile;
    return true;
}

std::shared_ptr<const LargeByteBuffer>
ReportArtifactStoreService::current_bytes() const {
    if (!bundle_) return {};

    switch (phase_) {
        case Phase::SubmitResult:
        case Phase::WaitResult:
            return bundle_->result;
        case Phase::SubmitOverview:
        case Phase::WaitOverview:
            return bundle_->overview;
        case Phase::SubmitRangeTile:
        case Phase::WaitRangeTile:
            return bundle_->range_tile;
        case Phase::SubmitManifest:
        case Phase::WaitManifest:
            return bundle_->key.kind == ReportArtifactKind::RangeTile
                ? manifest_bytes_
                : bundle_->manifest;
        default:
            return {};
    }
}

bool ReportArtifactStoreService::current_path(
    char *path,
    size_t path_size) const {
    if (!bundle_) return false;

    switch (phase_) {
        case Phase::SubmitResult:
        case Phase::WaitResult:
            return report_artifact_result_path(
                bundle_->key, path, path_size);
        case Phase::SubmitOverview:
        case Phase::WaitOverview:
            return report_artifact_overview_path(
                bundle_->key, path, path_size);
        case Phase::SubmitRangeTile:
        case Phase::WaitRangeTile:
            return report_artifact_tile_path(
                bundle_->key, path, path_size);
        case Phase::SubmitManifest:
        case Phase::WaitManifest:
            return report_artifact_manifest_path(
                bundle_->key.sleep_day, path, path_size);
        default:
            return false;
    }
}

bool ReportArtifactStoreService::submit_current() {
    if (!write_port_ || !bundle_) {
        fail("report_artifact_store_not_ready");
        return true;
    }

    char path[AC_STORAGE_PATH_MAX] = {};
    std::shared_ptr<const LargeByteBuffer> bytes = current_bytes();
    if (!bytes || !current_path(path, sizeof(path))) {
        fail("report_artifact_path_invalid");
        return true;
    }

    StorageAtomicWriteCommand command;
    command.path = path;
    command.bytes = std::move(bytes);
    command.lane = lane_;
    command.generation = generation_;

    const OperationSubmission submission = write_port_->request_write(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("report_artifact_write_rejected");
        return true;
    }

    write_ticket_ = submission.ticket;
    switch (phase_) {
        case Phase::SubmitResult:
            phase_ = Phase::WaitResult;
            break;
        case Phase::SubmitOverview:
            phase_ = Phase::WaitOverview;
            break;
        case Phase::SubmitRangeTile:
            phase_ = Phase::WaitRangeTile;
            break;
        case Phase::SubmitManifest:
            phase_ = Phase::WaitManifest;
            break;
        default:
            fail("report_artifact_store_phase_invalid");
            break;
    }
    return true;
}

bool ReportArtifactStoreService::finish_current() {
    if (!write_port_ || !write_ticket_.valid() || !bundle_) {
        fail("report_artifact_store_not_ready");
        return true;
    }

    StorageAtomicWriteCompletion completion;
    if (!write_port_->take_completion(write_ticket_, completion)) return false;
    write_ticket_ = {};

    std::shared_ptr<const LargeByteBuffer> expected = current_bytes();
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !expected || completion.bytes_written != expected->size()) {
        fail(completion.error[0] ? completion.error
                                 : "report_artifact_write_failed");
        return true;
    }
    status_.bytes_written += completion.bytes_written;

    switch (phase_) {
        case Phase::WaitResult:
            phase_ = Phase::SubmitOverview;
            status_.state = ReportArtifactStoreState::PublishingOverview;
            break;
        case Phase::WaitOverview:
        case Phase::WaitRangeTile:
            phase_ = Phase::SubmitManifest;
            status_.state = ReportArtifactStoreState::PublishingManifest;
            break;
        case Phase::WaitManifest:
            phase_ = Phase::Ready;
            status_.state = ReportArtifactStoreState::Ready;
            break;
        default:
            fail("report_artifact_store_phase_invalid");
            break;
    }
    return true;
}

bool ReportArtifactStoreService::poll() {
    switch (phase_) {
        case Phase::SubmitManifestRead:
            return submit_manifest_read();
        case Phase::WaitManifestRead:
            return finish_manifest_read();
        case Phase::SubmitResult:
        case Phase::SubmitOverview:
        case Phase::SubmitRangeTile:
        case Phase::SubmitManifest:
            return submit_current();
        case Phase::WaitResult:
        case Phase::WaitOverview:
        case Phase::WaitRangeTile:
        case Phase::WaitManifest:
            return finish_current();
        case Phase::Idle:
        case Phase::Ready:
        case Phase::Failed:
        case Phase::Cancelled:
            return false;
    }
    return false;
}

void ReportArtifactStoreService::fail(const char *error) {
    if (read_port_ && read_ticket_.valid()) {
        (void)read_port_->abandon(read_ticket_);
    }
    if (read_port_ && prepared_.valid()) {
        read_port_->release_prepared(prepared_);
    }
    if (write_port_ && write_ticket_.valid()) {
        (void)write_port_->abandon(write_ticket_);
    }

    read_ticket_ = {};
    prepared_ = {};
    write_ticket_ = {};
    bundle_.reset();
    manifest_bytes_.reset();
    phase_ = Phase::Failed;
    status_.state = ReportArtifactStoreState::Failed;
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "report_artifact_store_failed");
}

void ReportArtifactStoreService::cancel() {
    if (read_port_ && read_ticket_.valid()) {
        (void)read_port_->abandon(read_ticket_);
    }
    if (read_port_ && prepared_.valid()) {
        read_port_->release_prepared(prepared_);
    }
    if (write_port_ && write_ticket_.valid()) {
        (void)write_port_->abandon(write_ticket_);
    }
    if (phase_ == Phase::Idle) return;

    read_ticket_ = {};
    prepared_ = {};
    write_ticket_ = {};
    bundle_.reset();
    manifest_bytes_.reset();
    phase_ = Phase::Cancelled;
    status_.state = ReportArtifactStoreState::Cancelled;
    status_.error[0] = '\0';
}

void ReportArtifactStoreService::clear_operation() {
    read_ticket_ = {};
    prepared_ = {};
    write_ticket_ = {};
    bundle_.reset();
    manifest_bytes_.reset();
    generation_ = 0;
    lane_ = StorageAtomicWriteLane::Maintenance;
}

void ReportArtifactStoreService::reset() {
    if (status_.active()) cancel();
    clear_operation();
    phase_ = Phase::Idle;
    status_ = {};
}

std::shared_ptr<const ReportArtifactBundle>
ReportArtifactStoreService::published() const {
    return phase_ == Phase::Ready ? bundle_ : nullptr;
}

}  // namespace aircannect
