#include "storage_range_write_service.h"

#include <algorithm>
#include <new>
#include <stdio.h>

#include "large_object.h"
#include "storage_internal.h"

namespace aircannect {

StorageRangeWriteService::~StorageRangeWriteService() {
    if (job_) {
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
    if (!Storage::mounted()) return "storage_not_mounted";

    const bool exists = Storage::exists(command.path.c_str());
    if (!exists && command.offset == 0) {
        const auto parents = Storage::ensure_parent_directory_step(
            command.path.c_str(), job_->parent_cursor);

        if (parents == Storage::ParentDirectoryStep::Failed) {
            return "parent_create_failed";
        }
        if (parents == Storage::ParentDirectoryStep::More) return nullptr;
    }

    if (!exists && command.offset != 0) return "file_not_found";

    // Never fall back to "w" after a failed "r+": an unreadable existing
    // file must not be truncated. Directory and gap checks precede mutation.
    if (exists) {
        job_->output = Storage::open(command.path.c_str(), "r+");
        if (!job_->output) return "open_failed";
        if (job_->output.isDirectory()) return "not_a_file";
        if (command.offset > job_->output.size()) return "offset_past_end";
    }

    if (!exists || command.truncate) {
        if (job_->output) job_->output.close();
        job_->output = Storage::open(command.path.c_str(), "w");
        if (!job_->output) return "open_failed";
        if (job_->output.isDirectory()) return "not_a_file";
    }

    (void)job_->output.setBufferSize(512);
    if (!job_->output.seek(static_cast<uint32_t>(command.offset)) ||
        job_->output.position() != command.offset) {
        return "seek_failed";
    }

    job_->phase = Phase::Write;
    return nullptr;
}

const char *StorageRangeWriteService::write_locked() {
    const LargeByteBuffer &bytes = *job_->command.bytes;
    const size_t count = std::min(bytes.size() - job_->written,
                                  AC_STORAGE_RANGE_WRITE_STEP_BYTES);
    const size_t written = job_->output.write(
        bytes.data() + job_->written, count);

    job_->written += written;
    if (written != count) return "write_failed";
    if (job_->written == bytes.size()) job_->phase = Phase::Flush;

    return nullptr;
}

void StorageRangeWriteService::finish_locked(OperationOutcome outcome,
                                             const char *error) {
    uint64_t modified = 0;
    if (job_->output) {
        job_->output.flush();
        if (outcome.disposition == OperationDisposition::Succeeded) {
            const time_t last_write = job_->output.getLastWrite();
            if (last_write > 0) modified = static_cast<uint64_t>(last_write);
        }
        job_->output.close();
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
