#include "storage_file_client.h"

#include <utility>

#include "string_util.h"

namespace aircannect {

StoragePreparedFile::~StoragePreparedFile() {
    reset();
}

StoragePreparedFile::StoragePreparedFile(StoragePreparedFile &&other) noexcept {
    move_from(other);
}

StoragePreparedFile &StoragePreparedFile::operator=(StoragePreparedFile &&other) noexcept {
    if (this == &other) return *this;

    reset();
    move_from(other);
    return *this;
}

void StoragePreparedFile::adopt(StorageReadPort &port,
                                StoragePreparedRead prepared,
                                bool exists) {
    reset();
    port_ = &port;
    prepared_ = prepared;
    exists_ = exists;
}

void StoragePreparedFile::move_from(StoragePreparedFile &other) {
    port_ = other.port_;
    prepared_ = other.prepared_;
    exists_ = other.exists_;

    other.port_ = nullptr;
    other.prepared_ = {};
    other.exists_ = false;
}

PreparedByteRead StoragePreparedFile::read(size_t offset,
                                           uint8_t *buffer,
                                           size_t capacity) const {
    if (!port_ || !prepared_.valid() || !buffer || capacity == 0) return {};
    return port_->read_prepared(prepared_, offset, buffer, capacity);
}

void StoragePreparedFile::reset() {
    if (port_ && prepared_.valid()) port_->release_prepared(prepared_);
    port_ = nullptr;
    prepared_ = {};
    exists_ = false;
}

StorageFileClient::~StorageFileClient() {
    reset();
}

void StorageFileClient::begin(StorageReadPort &read_port,
                              StorageAtomicWritePort &write_port,
                              StoragePathPort &path_port) {
    reset();
    read_port_ = &read_port;
    write_port_ = &write_port;
    path_port_ = &path_port;
}

bool StorageFileClient::active() const {
    return phase_ != Phase::Idle && phase_ != Phase::Ready &&
           phase_ != Phase::Error;
}

OperationAdmission StorageFileClient::request_path_info(
    const char *path,
    size_t max_length,
    uint32_t generation,
    Operation operation) {
    if (active()) return OperationAdmission::Busy;
    reset();
    if (!path_port_ || !path || !path[0] || generation == 0 ||
        (operation == Operation::Read && !read_port_) ||
        (operation == Operation::Read && max_length == 0)) {
        return OperationAdmission::Rejected;
    }

    StoragePathCommand command;
    command.operation = StoragePathOperation::Stat;
    command.source = path;
    command.generation = generation;
    const OperationSubmission submission = path_port_->request(command);
    if (!submission.accepted()) return submission.admission;

    operation_ = operation;
    phase_ = Phase::WaitStat;
    ticket_ = submission.ticket;
    generation_ = generation;
    max_length_ = max_length;
    path_ = path;
    return OperationAdmission::Accepted;
}

OperationAdmission StorageFileClient::request_stat(const char *path,
                                                   uint32_t generation) {
    return request_path_info(path, 0, generation, Operation::Stat);
}

OperationAdmission StorageFileClient::request_read(const char *path,
                                                   size_t max_length,
                                                   uint32_t generation) {
    return request_path_info(path, max_length, generation, Operation::Read);
}

OperationAdmission StorageFileClient::request_replace(
    const char *path,
    std::shared_ptr<const LargeByteBuffer> bytes,
    uint32_t generation) {
    if (active()) return OperationAdmission::Busy;
    reset();
    if (!write_port_ || !path || !path[0] || !bytes || generation == 0) {
        return OperationAdmission::Rejected;
    }

    StorageAtomicWriteCommand command;
    command.path = path;
    command.bytes = std::move(bytes);
    command.lane = StorageAtomicWriteLane::Maintenance;
    command.generation = generation;
    const OperationSubmission submission = write_port_->request_write(command);
    if (!submission.accepted()) return submission.admission;

    operation_ = Operation::Replace;
    phase_ = Phase::WaitWrite;
    ticket_ = submission.ticket;
    generation_ = generation;
    path_ = path;
    return OperationAdmission::Accepted;
}

OperationAdmission StorageFileClient::request_remove(const char *path,
                                                     uint32_t generation) {
    if (active()) return OperationAdmission::Busy;
    reset();
    if (!path_port_ || !path || !path[0] || generation == 0) {
        return OperationAdmission::Rejected;
    }

    StoragePathCommand command;
    command.operation = StoragePathOperation::Remove;
    command.source = path;
    command.generation = generation;
    const OperationSubmission submission = path_port_->request(command);
    if (!submission.accepted()) return submission.admission;

    operation_ = Operation::Remove;
    phase_ = Phase::WaitRemove;
    ticket_ = submission.ticket;
    generation_ = generation;
    path_ = path;
    return OperationAdmission::Accepted;
}

StorageFileClientResult StorageFileClient::fail(const char *error) {
    copy_cstr(error_, sizeof(error_), error ? error : "storage_file_failed");
    phase_ = Phase::Error;
    ticket_ = {};
    return StorageFileClientResult::Error;
}

StorageFileClientResult StorageFileClient::poll_stat() {
    StoragePathCompletion completion;
    if (!path_port_->take_completion(ticket_, completion)) {
        return StorageFileClientResult::Waiting;
    }

    ticket_ = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        return fail(completion.error[0] ? completion.error
                                        : "storage_stat_failed");
    }

    info_.exists = completion.exists;
    info_.directory = completion.directory;
    info_.size = completion.size;
    info_.modified = completion.modified;
    if (operation_ == Operation::Stat) {
        phase_ = Phase::Ready;
        return StorageFileClientResult::Ready;
    }
    if (!info_.exists || info_.size == 0) {
        file_.adopt(*read_port_, {}, info_.exists);
        phase_ = Phase::Ready;
        return StorageFileClientResult::Ready;
    }
    if (info_.directory) return fail("storage_path_is_directory");
    if (info_.size > max_length_) return fail("storage_file_too_large");

    phase_ = Phase::SubmitRead;
    return submit_read();
}

StorageFileClientResult StorageFileClient::submit_read() {
    StorageReadCommand command;
    command.path = path_;
    command.length = static_cast<size_t>(info_.size);
    command.lane = StorageReadLane::Export;
    command.generation = generation_;
    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) {
        return StorageFileClientResult::Waiting;
    }
    if (!submission.accepted()) return fail("storage_read_rejected");

    ticket_ = submission.ticket;
    phase_ = Phase::WaitRead;
    return StorageFileClientResult::Waiting;
}

StorageFileClientResult StorageFileClient::poll_read() {
    StorageReadCompletion completion;
    if (!read_port_->take_completion(ticket_, completion)) {
        return StorageFileClientResult::Waiting;
    }

    ticket_ = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.prepared.valid() ||
        completion.prepared.length != info_.size) {
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        return fail("storage_read_failed");
    }

    file_.adopt(*read_port_, completion.prepared, true);
    phase_ = Phase::Ready;
    return StorageFileClientResult::Ready;
}

StorageFileClientResult StorageFileClient::poll_write() {
    StorageAtomicWriteCompletion completion;
    if (!write_port_->take_completion(ticket_, completion)) {
        return StorageFileClientResult::Waiting;
    }

    ticket_ = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        return fail(completion.error[0] ? completion.error
                                        : "storage_write_failed");
    }

    phase_ = Phase::Ready;
    return StorageFileClientResult::Ready;
}

StorageFileClientResult StorageFileClient::poll_remove() {
    StoragePathCompletion completion;
    if (!path_port_->take_completion(ticket_, completion)) {
        return StorageFileClientResult::Waiting;
    }

    ticket_ = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        return fail(completion.error[0] ? completion.error
                                        : "storage_remove_failed");
    }

    phase_ = Phase::Ready;
    return StorageFileClientResult::Ready;
}

StorageFileClientResult StorageFileClient::poll() {
    switch (phase_) {
        case Phase::Idle: return StorageFileClientResult::Idle;
        case Phase::WaitStat: return poll_stat();
        case Phase::SubmitRead: return submit_read();
        case Phase::WaitRead: return poll_read();
        case Phase::WaitWrite: return poll_write();
        case Phase::WaitRemove: return poll_remove();
        case Phase::Ready: return StorageFileClientResult::Ready;
        case Phase::Error: return StorageFileClientResult::Error;
    }
    return fail("storage_file_bad_phase");
}

StoragePreparedFile StorageFileClient::take_file() {
    if (phase_ != Phase::Ready || operation_ != Operation::Read) return {};

    StoragePreparedFile out = std::move(file_);
    reset();
    return out;
}

void StorageFileClient::reset() {
    if (ticket_.valid()) {
        if ((phase_ == Phase::WaitStat || phase_ == Phase::WaitRemove) &&
            path_port_) {
            (void)path_port_->abandon(ticket_);
        } else if (phase_ == Phase::WaitRead && read_port_) {
            (void)read_port_->abandon(ticket_);
        } else if (phase_ == Phase::WaitWrite && write_port_) {
            (void)write_port_->abandon(ticket_);
        }
    }

    file_.reset();
    operation_ = Operation::None;
    phase_ = Phase::Idle;
    ticket_ = {};
    generation_ = 0;
    max_length_ = 0;
    path_.clear();
    info_ = {};
    error_[0] = '\0';
}

}  // namespace aircannect
