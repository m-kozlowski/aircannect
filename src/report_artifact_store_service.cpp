#include "report_artifact_store_service.h"

#include <utility>

#include "string_util.h"

namespace aircannect {
namespace {

bool publishing_state(ReportArtifactStoreState state) {
    return state == ReportArtifactStoreState::PublishingResult ||
           state == ReportArtifactStoreState::PublishingOverview ||
           state == ReportArtifactStoreState::PublishingManifest;
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

OperationOutcome ReportArtifactStoreStatus::outcome() const {
    switch (state) {
        case ReportArtifactStoreState::Ready:
            return OperationOutcome::succeeded();
        case ReportArtifactStoreState::Cancelled:
            return OperationOutcome::cancelled();
        case ReportArtifactStoreState::Failed:
            return OperationOutcome::failed();
        default:
            return OperationOutcome::deferred(0);
    }
}

ReportArtifactStoreService::~ReportArtifactStoreService() {
    cancel();
}

void ReportArtifactStoreService::begin(StorageAtomicWritePort &write_port) {
    write_port_ = &write_port;
}

OperationAdmission ReportArtifactStoreService::start(
    std::shared_ptr<const ReportArtifactBundle> bundle,
    uint32_t generation,
    StorageAtomicWriteLane lane) {
    if (!write_port_) return OperationAdmission::Rejected;
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;
    if (!bundle || !bundle->valid() || generation == 0) {
        return OperationAdmission::Rejected;
    }

    bundle_ = std::move(bundle);
    generation_ = generation;
    lane_ = lane;
    phase_ = Phase::SubmitResult;
    status_ = {};
    status_.state = ReportArtifactStoreState::PublishingResult;
    status_.key = bundle_->key;
    return OperationAdmission::Accepted;
}

bool ReportArtifactStoreService::submit_current() {
    if (!write_port_ || !bundle_) {
        fail("report_artifact_store_not_ready");
        return true;
    }

    char path[AC_STORAGE_PATH_MAX] = {};
    std::shared_ptr<const LargeByteBuffer> bytes;
    bool path_valid = false;
    switch (phase_) {
        case Phase::SubmitResult:
            bytes = bundle_->result;
            path_valid = report_artifact_result_path(
                bundle_->key, path, sizeof(path));
            break;
        case Phase::SubmitOverview:
            bytes = bundle_->overview;
            path_valid = report_artifact_overview_path(
                bundle_->key, path, sizeof(path));
            break;
        case Phase::SubmitManifest:
            bytes = bundle_->manifest;
            path_valid = report_artifact_manifest_path(
                bundle_->key.sleep_day, path, sizeof(path));
            break;
        default:
            break;
    }
    if (!bytes || !path_valid) {
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

    std::shared_ptr<const LargeByteBuffer> expected;
    switch (phase_) {
        case Phase::WaitResult:
            expected = bundle_->result;
            break;
        case Phase::WaitOverview:
            expected = bundle_->overview;
            break;
        case Phase::WaitManifest:
            expected = bundle_->manifest;
            break;
        default:
            fail("report_artifact_store_phase_invalid");
            return true;
    }

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
        case Phase::SubmitResult:
        case Phase::SubmitOverview:
        case Phase::SubmitManifest:
            return submit_current();
        case Phase::WaitResult:
        case Phase::WaitOverview:
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
    if (write_port_ && write_ticket_.valid()) {
        (void)write_port_->abandon(write_ticket_);
    }
    write_ticket_ = {};
    bundle_.reset();
    phase_ = Phase::Failed;
    status_.state = ReportArtifactStoreState::Failed;
    copy_cstr(status_.error,
              sizeof(status_.error),
              error ? error : "report_artifact_store_failed");
}

void ReportArtifactStoreService::cancel() {
    if (write_port_ && write_ticket_.valid()) {
        (void)write_port_->abandon(write_ticket_);
    }
    if (phase_ == Phase::Idle) return;

    write_ticket_ = {};
    bundle_.reset();
    phase_ = Phase::Cancelled;
    status_.state = ReportArtifactStoreState::Cancelled;
    status_.error[0] = '\0';
}

void ReportArtifactStoreService::clear_operation() {
    write_ticket_ = {};
    bundle_.reset();
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
