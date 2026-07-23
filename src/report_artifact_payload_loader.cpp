#include "report_artifact_payload_loader.h"

#include <algorithm>

#include "board_report.h"
#include "crc32.h"
#include "string_util.h"

namespace aircannect {

bool ReportArtifactPayloadLoadStatus::active() const {
    return state == ReportArtifactPayloadLoadState::Submitting ||
           state == ReportArtifactPayloadLoadState::Waiting ||
           state == ReportArtifactPayloadLoadState::Copying;
}

bool ReportArtifactPayloadLoadStatus::terminal() const {
    return state == ReportArtifactPayloadLoadState::Ready ||
           state == ReportArtifactPayloadLoadState::Error ||
           state == ReportArtifactPayloadLoadState::Cancelled;
}

ReportArtifactPayloadLoader::~ReportArtifactPayloadLoader() {
    cancel();
}

void ReportArtifactPayloadLoader::begin(StorageReadPort &read_port) {
    read_port_ = &read_port;
}

OperationAdmission ReportArtifactPayloadLoader::start(
    const ReportArtifactDescriptor &artifact,
    uint32_t generation,
    StorageReadLane lane) {
    if (!read_port_) return OperationAdmission::Rejected;
    if (status_.active()) return OperationAdmission::Busy;
    if (!artifact.valid() || artifact.size == 0 ||
        artifact.size > SIZE_MAX || generation == 0) {
        return OperationAdmission::Rejected;
    }

    reset();
    buffer_ = LargeByteBuffer::allocate(static_cast<size_t>(artifact.size));
    if (!buffer_) return OperationAdmission::Rejected;

    generation_ = generation;
    lane_ = lane;
    crc_state_ = crc32_ieee_initial_state();
    status_.artifact = artifact;
    status_.state = ReportArtifactPayloadLoadState::Submitting;
    return OperationAdmission::Accepted;
}

bool ReportArtifactPayloadLoader::submit_chunk() {
    if (!read_port_ || !buffer_ || offset_ >= buffer_->size()) {
        fail("report_payload_load_not_ready");
        return true;
    }

    char path[AC_STORAGE_PATH_MAX] = {};
    if (!status_.artifact.path(path, sizeof(path))) {
        fail("report_payload_path_invalid");
        return true;
    }

    chunk_length_ = std::min(
        buffer_->size() - offset_, AC_STORAGE_PREPARED_READ_MAX_BYTES);
    chunk_copied_ = 0;

    StorageReadCommand command;
    command.path = path;
    command.offset = offset_;
    command.length = chunk_length_;
    command.lane = lane_;
    command.generation = generation_;

    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("report_payload_read_rejected");
        return true;
    }

    ticket_ = submission.ticket;
    status_.state = ReportArtifactPayloadLoadState::Waiting;
    return true;
}

bool ReportArtifactPayloadLoader::finish_chunk_read() {
    if (!read_port_ || !ticket_.valid()) {
        fail("report_payload_load_not_ready");
        return true;
    }

    StorageReadCompletion completion;
    if (!read_port_->take_completion(ticket_, completion)) return false;
    ticket_ = {};

    if (completion.outcome.disposition !=
            OperationDisposition::Succeeded ||
        !completion.prepared.valid() ||
        completion.prepared.length != chunk_length_) {
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        fail(completion.error[0] ? completion.error
                                 : "report_payload_read_failed");
        return true;
    }

    prepared_ = completion.prepared;
    status_.state = ReportArtifactPayloadLoadState::Copying;
    return true;
}

bool ReportArtifactPayloadLoader::copy_chunk() {
    if (!read_port_ || !prepared_.valid() || !buffer_ ||
        chunk_copied_ >= chunk_length_) {
        fail("report_payload_copy_not_ready");
        return true;
    }

    const size_t wanted = std::min(
        chunk_length_ - chunk_copied_, AC_REPORT_PAYLOAD_LOAD_COPY_BYTES);
    uint8_t *destination =
        buffer_->data() + offset_ + chunk_copied_;
    const PreparedByteRead read = read_port_->read_prepared(
        prepared_, chunk_copied_, destination, wanted);
    if (read.state == PreparedByteReadState::Retry) return false;
    if (read.state != PreparedByteReadState::Data ||
        read.bytes == 0 || read.bytes > wanted) {
        fail("report_payload_copy_failed");
        return true;
    }

    crc_state_ = crc32_ieee_update_state(
        crc_state_, destination, read.bytes);
    chunk_copied_ += read.bytes;
    status_.bytes_loaded = offset_ + chunk_copied_;
    if (chunk_copied_ < chunk_length_) return true;

    read_port_->release_prepared(prepared_);
    prepared_ = {};
    offset_ += chunk_length_;
    chunk_length_ = 0;
    chunk_copied_ = 0;

    if (offset_ == buffer_->size()) {
        finish();
    } else {
        status_.state = ReportArtifactPayloadLoadState::Submitting;
    }
    return true;
}

void ReportArtifactPayloadLoader::finish() {
    const uint32_t crc = crc32_ieee_finish_state(crc_state_);
    if (crc != status_.artifact.crc32) {
        fail("report_payload_crc_mismatch");
        return;
    }

    completed_ = LargeByteBuffer::freeze(std::move(buffer_));
    if (!completed_) {
        fail("report_payload_publish_failed");
        return;
    }
    status_.state = ReportArtifactPayloadLoadState::Ready;
}

bool ReportArtifactPayloadLoader::poll() {
    switch (status_.state) {
        case ReportArtifactPayloadLoadState::Submitting:
            return submit_chunk();
        case ReportArtifactPayloadLoadState::Waiting:
            return finish_chunk_read();
        case ReportArtifactPayloadLoadState::Copying:
            return copy_chunk();
        case ReportArtifactPayloadLoadState::Idle:
        case ReportArtifactPayloadLoadState::Ready:
        case ReportArtifactPayloadLoadState::Error:
        case ReportArtifactPayloadLoadState::Cancelled:
            return false;
    }
    return false;
}

void ReportArtifactPayloadLoader::fail(const char *error) {
    if (read_port_ && ticket_.valid()) {
        (void)read_port_->abandon(ticket_);
    }
    if (read_port_ && prepared_.valid()) {
        read_port_->release_prepared(prepared_);
    }

    ticket_ = {};
    prepared_ = {};
    buffer_.reset();
    completed_.reset();
    status_.state = ReportArtifactPayloadLoadState::Error;
    copy_cstr(status_.error, sizeof(status_.error),
              error ? error : "report_payload_load_failed");
}

void ReportArtifactPayloadLoader::cancel() {
    if (read_port_ && ticket_.valid()) {
        (void)read_port_->abandon(ticket_);
    }
    if (read_port_ && prepared_.valid()) {
        read_port_->release_prepared(prepared_);
    }
    if (status_.state == ReportArtifactPayloadLoadState::Idle) return;

    ticket_ = {};
    prepared_ = {};
    buffer_.reset();
    completed_.reset();
    status_.state = ReportArtifactPayloadLoadState::Cancelled;
    status_.error[0] = '\0';
}

void ReportArtifactPayloadLoader::release_operation() {
    ticket_ = {};
    prepared_ = {};
    buffer_.reset();
    completed_.reset();
    generation_ = 0;
    crc_state_ = 0;
    offset_ = 0;
    chunk_length_ = 0;
    chunk_copied_ = 0;
    lane_ = StorageReadLane::Maintenance;
}

void ReportArtifactPayloadLoader::reset() {
    if (status_.active()) cancel();
    release_operation();
    status_ = {};
}

std::shared_ptr<const LargeByteBuffer>
ReportArtifactPayloadLoader::take_completed() {
    if (status_.state != ReportArtifactPayloadLoadState::Ready ||
        !completed_) {
        return {};
    }

    std::shared_ptr<const LargeByteBuffer> out = std::move(completed_);
    release_operation();
    status_ = {};
    return out;
}

}  // namespace aircannect
