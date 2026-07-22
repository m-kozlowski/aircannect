#include "report_artifact_lookup_service.h"

#include <memory>
#include <string.h>

#include "string_util.h"

namespace aircannect {

bool ReportArtifactLookupStatus::active() const {
    return state == ReportArtifactLookupState::SubmitManifest ||
           state == ReportArtifactLookupState::WaitManifest;
}

bool ReportArtifactLookupStatus::terminal() const {
    return state == ReportArtifactLookupState::Ready ||
           state == ReportArtifactLookupState::MissingManifest ||
           state == ReportArtifactLookupState::MissingArtifact ||
           state == ReportArtifactLookupState::Failed ||
           state == ReportArtifactLookupState::Cancelled;
}

ReportArtifactLookupService::~ReportArtifactLookupService() {
    cancel();
}

void ReportArtifactLookupService::begin(StorageReadPort &read_port) {
    reset();
    read_port_ = &read_port;
}

OperationAdmission ReportArtifactLookupService::start(
    const ReportArtifactKey &request,
    uint32_t generation,
    StorageReadLane lane) {
    if (!read_port_ || !request.valid() || generation == 0) {
        return OperationAdmission::Rejected;
    }
    if (status_.state != ReportArtifactLookupState::Idle) {
        return OperationAdmission::Busy;
    }

    status_ = {};
    status_.state = ReportArtifactLookupState::SubmitManifest;
    status_.request = request;
    status_.generation = generation;
    lane_ = lane;
    availability_ = {};
    return OperationAdmission::Accepted;
}

bool ReportArtifactLookupService::submit_manifest() {
    char path[AC_STORAGE_PATH_MAX] = {};
    if (!report_artifact_manifest_path(
            status_.request.sleep_day, path, sizeof(path))) {
        finish(ReportArtifactLookupState::Failed,
               "report_artifact_manifest_path_invalid");
        return true;
    }

    StorageReadCommand command;
    command.path = path;
    command.length = ReportArtifactManifestCodec::MaxBytes;
    command.lane = lane_;
    command.generation = status_.generation;

    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        finish(ReportArtifactLookupState::Failed,
               "report_artifact_manifest_read_rejected");
        return true;
    }

    ticket_ = submission.ticket;
    status_.state = ReportArtifactLookupState::WaitManifest;
    return true;
}

bool ReportArtifactLookupService::finish_manifest() {
    if (!prepared_.valid()) {
        StorageReadCompletion completion;
        if (!read_port_->take_completion(ticket_, completion)) return false;
        ticket_ = {};

        if (completion.outcome.disposition == OperationDisposition::Cancelled) {
            if (completion.prepared.valid()) {
                read_port_->release_prepared(completion.prepared);
            }
            finish(ReportArtifactLookupState::Cancelled, nullptr);
            return true;
        }
        if (completion.outcome.disposition != OperationDisposition::Succeeded) {
            if (completion.prepared.valid()) {
                read_port_->release_prepared(completion.prepared);
            }
            if (strcmp(completion.error, "read_open_failed") == 0) {
                finish(ReportArtifactLookupState::MissingManifest,
                       "report_artifact_manifest_missing");
            } else {
                finish(ReportArtifactLookupState::Failed,
                       completion.error[0]
                           ? completion.error
                           : "report_artifact_manifest_read_failed");
            }
            return true;
        }
        if (!completion.prepared.valid() ||
            completion.prepared.length <
                ReportArtifactManifestCodec::HeaderBytes ||
            completion.prepared.length > ReportArtifactManifestCodec::MaxBytes) {
            if (completion.prepared.valid()) {
                read_port_->release_prepared(completion.prepared);
            }
            finish(ReportArtifactLookupState::MissingManifest,
                   "report_artifact_manifest_invalid");
            return true;
        }

        prepared_ = completion.prepared;
    }

    std::unique_ptr<LargeByteBuffer> bytes =
        LargeByteBuffer::allocate(prepared_.length);
    if (!bytes) {
        finish(ReportArtifactLookupState::Failed,
               "report_artifact_manifest_allocation_failed");
        return true;
    }

    const PreparedByteRead read = read_port_->read_prepared(
        prepared_, 0, bytes->data(), bytes->size());
    if (read.state == PreparedByteReadState::Retry) return false;

    release_prepared();
    if (read.state != PreparedByteReadState::Data ||
        read.bytes != bytes->size()) {
        finish(ReportArtifactLookupState::Failed,
               "report_artifact_manifest_short_read");
        return true;
    }

    ReportArtifactManifestView manifest;
    if (!ReportArtifactManifestCodec::decode(
            bytes->data(), bytes->size(), manifest)) {
        finish(ReportArtifactLookupState::MissingManifest,
               "report_artifact_manifest_invalid");
        return true;
    }
    if (manifest.key.sleep_day != status_.request.sleep_day ||
        manifest.key.source_revision != status_.request.source_revision) {
        finish(ReportArtifactLookupState::MissingManifest,
               "report_artifact_manifest_stale");
        return true;
    }
    if (!availability_.load(manifest, status_.request)) {
        finish(ReportArtifactLookupState::MissingManifest,
               "report_artifact_manifest_invalid");
        return true;
    }

    if (availability_.requested_ready()) {
        finish(ReportArtifactLookupState::Ready, nullptr, true);
    } else {
        finish(ReportArtifactLookupState::MissingArtifact,
               "report_artifact_missing",
               true);
    }
    return true;
}

bool ReportArtifactLookupService::poll() {
    switch (status_.state) {
        case ReportArtifactLookupState::SubmitManifest:
            return submit_manifest();
        case ReportArtifactLookupState::WaitManifest:
            return finish_manifest();
        case ReportArtifactLookupState::Idle:
        case ReportArtifactLookupState::Ready:
        case ReportArtifactLookupState::MissingManifest:
        case ReportArtifactLookupState::MissingArtifact:
        case ReportArtifactLookupState::Failed:
        case ReportArtifactLookupState::Cancelled:
            return false;
    }
    return false;
}

void ReportArtifactLookupService::finish(
    ReportArtifactLookupState state,
    const char *error,
    bool keep_availability) {
    if (read_port_ && ticket_.valid()) {
        (void)read_port_->abandon(ticket_);
    }
    ticket_ = {};
    release_prepared();
    if (!keep_availability) availability_ = {};

    status_.state = state;
    copy_cstr(status_.error, sizeof(status_.error), error ? error : "");
}

void ReportArtifactLookupService::release_prepared() {
    if (!read_port_ || !prepared_.valid()) return;

    const StoragePreparedRead prepared = prepared_;
    prepared_ = {};
    read_port_->release_prepared(prepared);
}

void ReportArtifactLookupService::cancel() {
    if (!status_.active()) return;
    finish(ReportArtifactLookupState::Cancelled, nullptr);
}

void ReportArtifactLookupService::reset() {
    if (status_.active()) cancel();
    ticket_ = {};
    release_prepared();
    lane_ = StorageReadLane::Report;
    status_ = {};
    availability_ = {};
}

}  // namespace aircannect
