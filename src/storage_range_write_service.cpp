#include "storage_range_write_service.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "large_object.h"
#include "storage_internal.h"

namespace aircannect {

StorageRangeWriteService::~StorageRangeWriteService() {
    if (job_) {
        if (job_->output >= 0) ::close(job_->output);
        LargeObject::destroy(job_);
    }

    if (mutex_) vSemaphoreDelete(mutex_);
}

bool StorageRangeWriteService::begin(WakeCallback wake) {
    wake_ = wake;
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;

    if (!job_) {
        job_ = LargeObject::create<Job>();
    }

    return job_ != nullptr;
}

void StorageRangeWriteService::set_task_available(bool available) {
    task_available_.store(available, std::memory_order_release);
    if (available) wake();
}

bool StorageRangeWriteService::ready() const {
    return mutex_ && job_ && task_available_.load(std::memory_order_acquire);
}

bool StorageRangeWriteService::lock() const {
    return mutex_ && xSemaphoreTake(mutex_, 0) == pdTRUE;
}

void StorageRangeWriteService::unlock() const {
    xSemaphoreGive(mutex_);
}

void StorageRangeWriteService::wake() const {
    if (wake_) wake_();
}

bool StorageRangeWriteService::apply_abandon_locked() {
    const uint64_t requested = abandon_request_.exchange(
        0, std::memory_order_acq_rel);

    if (!requested) return false;

    const OperationTicket ticket{static_cast<uint32_t>(requested),
                                 static_cast<uint32_t>(requested >> 32)};

    if (job_->ticket == ticket) {
        job_->abandoned = true;
    } else if (completion_.ticket == ticket) {
        completion_ = {};
    }

    return true;
}

OperationSubmission StorageRangeWriteService::request_write(
    const StorageRangeWriteCommand &command) {
    if (!command.valid()) return OperationSubmission::rejected();
    if (!ready() || !lock()) return OperationSubmission::busy();

    (void)apply_abandon_locked();
    if (job_->ticket.valid() || completion_.ticket.valid()) {
        unlock();
        return OperationSubmission::busy();
    }

    if (++next_ticket_id_ == 0) ++next_ticket_id_;
    const OperationTicket ticket{next_ticket_id_, command.generation};
    job_->command = command;
    job_->ticket = ticket;
    unlock();

    wake();
    return OperationSubmission::accepted(ticket);
}

bool StorageRangeWriteService::abandon(OperationTicket ticket) {
    if (!ready() || !ticket.valid()) return false;

    const uint64_t requested =
        (static_cast<uint64_t>(ticket.generation) << 32) | ticket.id;
    uint64_t previous = 0;
    const bool accepted = abandon_request_.compare_exchange_strong(
        previous, requested, std::memory_order_acq_rel);

    if (!accepted && previous != requested) return false;

    wake();
    return true;
}

bool StorageRangeWriteService::take_completion(
    OperationTicket ticket, StorageRangeWriteCompletion &completion) {
    if (!ready() || !ticket.valid() || !lock()) return false;

    (void)apply_abandon_locked();
    const bool found = completion_.ticket == ticket;
    if (found) {
        completion = completion_;
        completion_ = {};
    }

    unlock();
    return found;
}

const char *StorageRangeWriteService::open_locked() {
    const StorageRangeWriteCommand &command = job_->command;
    const int flags = O_RDWR | (command.offset == 0 ? O_CREAT : 0) |
                      (command.truncate ? O_TRUNC : 0);
    job_->output = Storage::open_descriptor(command.path.c_str(), flags);

    if (job_->output < 0) {
        const int error = errno;
        if (command.offset != 0) {
            return error == ENOENT ? "file_not_found" : "open_failed";
        }
        if (error != ENOENT && error != ENOTDIR) return "open_failed";

        const auto parents = Storage::ensure_parent_directory_step(
            command.path.c_str(), job_->parent_cursor);

        if (parents == Storage::ParentDirectoryStep::Failed) {
            return "parent_create_failed";
        }
        if (parents == Storage::ParentDirectoryStep::More) return nullptr;

        job_->output = Storage::open_descriptor(command.path.c_str(), flags);
        if (job_->output < 0) return "open_failed";
    }

    struct stat info {};
    if (::fstat(job_->output, &info) != 0) return "stat_failed";
    if (!S_ISREG(info.st_mode)) return "not_a_file";
    if (command.offset > static_cast<uint64_t>(info.st_size)) {
        return "offset_past_end";
    }

    if (::lseek(job_->output, command.offset, SEEK_SET) !=
        static_cast<off_t>(command.offset)) {
        return "seek_failed";
    }

    job_->phase = Phase::Write;
    return nullptr;
}

const char *StorageRangeWriteService::write_locked() {
    const LargeByteBuffer &bytes = *job_->command.bytes;
    const size_t count = std::min(bytes.size() - job_->written,
                                  AC_STORAGE_RANGE_WRITE_STEP_BYTES);
    const size_t written = Storage::write_buffer(
        job_->output, bytes.data() + job_->written, count);

    job_->written += written;
    if (written != count) return "write_failed";
    if (job_->written == bytes.size()) job_->phase = Phase::Flush;

    return nullptr;
}

void StorageRangeWriteService::finish_locked(OperationOutcome outcome,
                                             const char *error) {
    uint64_t modified = 0;
    if (job_->output >= 0) {
        if (::fsync(job_->output) != 0 &&
            outcome.disposition == OperationDisposition::Succeeded) {
            outcome = OperationOutcome::failed();
            error = "flush_failed";
        }

        if (outcome.disposition == OperationDisposition::Succeeded) {
            modified = Storage::file_modified(job_->command.path.c_str());
        }
        ::close(job_->output);
    }

    // Catch cancellation posted during a blocking write or flush as well.
    (void)apply_abandon_locked();
    if (!job_->abandoned) {
        completion_.ticket = job_->ticket;
        completion_.outcome = outcome;
        completion_.bytes_written = job_->written;
        completion_.modified = modified;

        snprintf(completion_.error, sizeof(completion_.error), "%s",
                 error ? error : "");
    }

    job_->~Job();
    new (job_) Job();
}

bool StorageRangeWriteService::step(StorageAtomicWriteLane lane) {
    if (!ready() || !lock()) return false;

    const bool abandoned = apply_abandon_locked();
    if (!job_->ticket.valid() ||
        (!job_->abandoned && job_->command.lane != lane)) {
        unlock();
        return abandoned;
    }

    const char *error = nullptr;
    if (job_->abandoned) {
        finish_locked(OperationOutcome::cancelled());
    } else if (!Storage::mounted()) {
        finish_locked(OperationOutcome::failed(), "storage_not_mounted");
    } else {
        switch (job_->phase) {
            case Phase::Open:
                error = open_locked();
                break;
            case Phase::Write:
                error = write_locked();
                break;
            case Phase::Flush:
                finish_locked(OperationOutcome::succeeded());
                break;
        }

        if (error) finish_locked(OperationOutcome::failed(), error);
    }

    unlock();
    return true;
}

}  // namespace aircannect
