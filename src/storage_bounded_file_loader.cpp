#include "storage_bounded_file_loader.h"

#include <algorithm>
#include <string.h>
#include <utility>

#include "board_report.h"
#include "storage_path.h"
#include "string_util.h"

namespace aircannect {

namespace {
constexpr size_t READ_BYTES = 64 * 1024;
}

bool StorageBoundedFileLoadStatus::active() const {
    return state == StorageBoundedFileLoadState::Submitting ||
           state == StorageBoundedFileLoadState::Waiting ||
           state == StorageBoundedFileLoadState::Copying;
}

bool StorageBoundedFileLoadStatus::terminal() const {
    return state == StorageBoundedFileLoadState::Ready ||
           state == StorageBoundedFileLoadState::Missing ||
           state == StorageBoundedFileLoadState::Failed ||
           state == StorageBoundedFileLoadState::Cancelled;
}

StorageBoundedFileLoader::~StorageBoundedFileLoader() {
    cancel();
}

void StorageBoundedFileLoader::begin(StorageReadPort &read_port) {
    reset();
    read_port_ = &read_port;
}

OperationAdmission StorageBoundedFileLoader::start(
    const char *path,
    size_t maximum_size,
    uint32_t generation,
    StorageReadLane lane) {
    if (status_.state != StorageBoundedFileLoadState::Idle) {
        return OperationAdmission::Busy;
    }
    if (!read_port_ || !path || !storage_user_path_valid(path) ||
        maximum_size == 0 ||
        maximum_size > AC_STORAGE_PREPARED_READ_MAX_BYTES ||
        generation == 0) {
        return OperationAdmission::Rejected;
    }

    generation_ = generation;
    maximum_size_ = maximum_size;
    copy_cstr(path_, sizeof(path_), path);
    status_ = {};
    status_.state = StorageBoundedFileLoadState::Submitting;
    status_.lane = lane;
    return OperationAdmission::Accepted;
}

bool StorageBoundedFileLoader::submit() {
    StorageReadCommand command;
    command.path = path_;
    command.offset = copied_;
    command.length = std::min(
        READ_BYTES, (buffer_ ? buffer_->size() : maximum_size_) - copied_);
    command.lane = status_.lane;
    command.generation = generation_;
    command.retain_handle = true;

    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        finish(StorageBoundedFileLoadState::Failed,
               "storage_file_read_rejected");
        return true;
    }

    ticket_ = submission.ticket;
    handle_requested_ = true;
    status_.state = StorageBoundedFileLoadState::Waiting;
    return true;
}

bool StorageBoundedFileLoader::finish_read() {
    StorageReadCompletion completion;
    if (!read_port_->take_completion(ticket_, completion)) return false;
    ticket_ = {};

    if (completion.outcome.disposition == OperationDisposition::Cancelled) {
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        finish(StorageBoundedFileLoadState::Cancelled, nullptr);
        return true;
    }
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        finish(copied_ == 0 && strcmp(completion.error, "read_not_found") == 0
                   ? StorageBoundedFileLoadState::Missing
                   : StorageBoundedFileLoadState::Failed,
               completion.error[0]
                   ? completion.error
                   : "storage_file_read_failed");
        return true;
    }
    prepared_ = completion.prepared;
    prepared_copied_ = 0;
    if (buffer_ && (completion.file_size != status_.file_size ||
                    completion.modified != status_.modified)) {
        finish(StorageBoundedFileLoadState::Failed, "storage_file_changed");
        return true;
    }

    const size_t length = static_cast<size_t>(
        std::min<uint64_t>(completion.file_size, maximum_size_));
    if (!prepared_.valid() || length <= copied_ ||
        prepared_.length != std::min(READ_BYTES, length - copied_)) {
        finish(StorageBoundedFileLoadState::Failed,
               "storage_file_size_invalid");
        return true;
    }

    if (!buffer_) {
        buffer_ = LargeByteBuffer::allocate(length);
        if (!buffer_) {
            finish(StorageBoundedFileLoadState::Failed,
                   "storage_file_allocation_failed");
            return true;
        }

        status_.file_size = completion.file_size;
        status_.modified = completion.modified;
    }

    status_.state = StorageBoundedFileLoadState::Copying;
    return copy();
}

bool StorageBoundedFileLoader::copy() {
    if (!prepared_.valid() || !buffer_ || copied_ >= buffer_->size()) {
        finish(StorageBoundedFileLoadState::Failed,
               "storage_file_copy_not_ready");
        return true;
    }

    const size_t wanted = std::min(
        prepared_.length - prepared_copied_, AC_REPORT_FILE_LOAD_COPY_BYTES);
    const PreparedByteRead read = read_port_->read_prepared(
        prepared_, prepared_copied_, buffer_->data() + copied_, wanted);
    if (read.state == PreparedByteReadState::Retry) return false;
    if (read.state != PreparedByteReadState::Data ||
        read.bytes == 0 || read.bytes > wanted) {
        finish(StorageBoundedFileLoadState::Failed,
               "storage_file_copy_failed");
        return true;
    }

    copied_ += read.bytes;
    prepared_copied_ += read.bytes;
    status_.bytes_loaded = copied_;
    if (prepared_copied_ < prepared_.length) return true;

    release_prepared();
    if (copied_ < buffer_->size()) {
        status_.state = StorageBoundedFileLoadState::Submitting;
        return true;
    }

    completed_ = LargeByteBuffer::freeze(std::move(buffer_));
    if (!completed_) {
        finish(StorageBoundedFileLoadState::Failed,
               "storage_file_publish_failed");
        return true;
    }

    finish(StorageBoundedFileLoadState::Ready, nullptr);
    return true;
}

bool StorageBoundedFileLoader::poll() {
    switch (status_.state) {
        case StorageBoundedFileLoadState::Submitting:
            return submit();
        case StorageBoundedFileLoadState::Waiting:
            return finish_read();
        case StorageBoundedFileLoadState::Copying:
            return copy();
        case StorageBoundedFileLoadState::Idle:
        case StorageBoundedFileLoadState::Ready:
        case StorageBoundedFileLoadState::Missing:
        case StorageBoundedFileLoadState::Failed:
        case StorageBoundedFileLoadState::Cancelled:
            return false;
    }
    return false;
}

void StorageBoundedFileLoader::finish(StorageBoundedFileLoadState state,
                                      const char *error) {
    if (read_port_ && ticket_.valid()) {
        (void)read_port_->abandon(ticket_);
    }
    ticket_ = {};
    release_prepared();
    if (handle_requested_) {
        read_port_->release_handle();
        handle_requested_ = false;
    }
    buffer_.reset();
    if (state != StorageBoundedFileLoadState::Ready) completed_.reset();

    status_.state = state;
    copy_cstr(status_.error, sizeof(status_.error), error ? error : "");
}

void StorageBoundedFileLoader::release_prepared() {
    if (!read_port_ || !prepared_.valid()) return;

    const StoragePreparedRead prepared = prepared_;
    prepared_ = {};
    read_port_->release_prepared(prepared);
}

void StorageBoundedFileLoader::cancel() {
    if (!status_.active()) return;
    finish(StorageBoundedFileLoadState::Cancelled, nullptr);
}

void StorageBoundedFileLoader::clear_operation() {
    ticket_ = {};
    release_prepared();
    buffer_.reset();
    completed_.reset();
    generation_ = 0;
    maximum_size_ = 0;
    copied_ = 0;
    prepared_copied_ = 0;
    path_[0] = '\0';
}

void StorageBoundedFileLoader::reset() {
    if (status_.active()) cancel();
    clear_operation();
    status_ = {};
}

std::shared_ptr<const LargeByteBuffer>
StorageBoundedFileLoader::take_completed() {
    if (status_.state != StorageBoundedFileLoadState::Ready || !completed_) {
        return {};
    }

    std::shared_ptr<const LargeByteBuffer> out = std::move(completed_);
    clear_operation();
    status_ = {};
    return out;
}

}  // namespace aircannect
