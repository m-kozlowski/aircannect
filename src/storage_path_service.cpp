#include "storage_path_service.h"

#include <FS.h>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {

StoragePathService::~StoragePathService() {
    Memory::free(jobs_);
    Memory::free(completions_);
    if (lock_) vSemaphoreDelete(lock_);
}

bool StoragePathService::begin(WakeCallback wake) {
    if (lock_) return jobs_ && completions_;

    wake_ = wake;
    lock_ = xSemaphoreCreateMutex();
    jobs_ = static_cast<JobSlot *>(
        Memory::calloc_large(Capacity, sizeof(JobSlot), false));
    completions_ = static_cast<CompletionSlot *>(
        Memory::calloc_large(Capacity, sizeof(CompletionSlot), false));
    if (lock_ && jobs_ && completions_) return true;

    Memory::free(jobs_);
    Memory::free(completions_);
    jobs_ = nullptr;
    completions_ = nullptr;
    if (lock_) vSemaphoreDelete(lock_);
    lock_ = nullptr;

    Log::logf(CAT_STORAGE, LOG_ERROR,
              "path operation service unavailable\n");
    return false;
}

void StoragePathService::set_task_available(bool available) {
    task_available_ = available;
    if (available) wake();
}

bool StoragePathService::ready() const {
    return lock_ && jobs_ && completions_ && task_available_;
}

bool StoragePathService::lock(uint32_t timeout_ms) const {
    return lock_ &&
           xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StoragePathService::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StoragePathService::wake() const {
    if (wake_) wake_();
}

StoragePathService::JobSlot *StoragePathService::find_job_locked(
    OperationTicket ticket) {
    for (size_t i = 0; i < Capacity; ++i) {
        JobSlot &job = jobs_[i];
        if (job.used && job.ticket == ticket) return &job;
    }
    return nullptr;
}

StoragePathService::CompletionSlot *
StoragePathService::find_completion_locked(OperationTicket ticket) {
    for (size_t i = 0; i < Capacity; ++i) {
        CompletionSlot &slot = completions_[i];
        if (slot.used && slot.completion.ticket == ticket) return &slot;
    }
    return nullptr;
}

StoragePathService::CompletionSlot *
StoragePathService::find_free_completion_locked() {
    for (size_t i = 0; i < Capacity; ++i) {
        CompletionSlot &slot = completions_[i];
        if (!slot.used) return &slot;
    }
    return nullptr;
}

size_t StoragePathService::select_job_locked() const {
    size_t selected = SIZE_MAX;
    uint64_t sequence = UINT64_MAX;
    for (size_t i = 0; i < Capacity; ++i) {
        const JobSlot &job = jobs_[i];
        if (!job.used || job.started || job.sequence >= sequence) continue;

        selected = i;
        sequence = job.sequence;
    }
    return selected;
}

size_t StoragePathService::completion_count_locked() const {
    size_t count = 0;
    for (size_t i = 0; i < Capacity; ++i) {
        const CompletionSlot &slot = completions_[i];
        if (slot.used) count++;
    }
    return count;
}

OperationTicket StoragePathService::next_ticket_locked(uint32_t generation) {
    next_ticket_id_++;
    if (next_ticket_id_ == 0) next_ticket_id_++;
    return {next_ticket_id_, generation};
}

OperationSubmission StoragePathService::request(
    const StoragePathCommand &command) {
    if (!command.valid() || command.source.size() >= AC_STORAGE_PATH_MAX ||
        command.destination.size() >= AC_STORAGE_PATH_MAX) {
        return OperationSubmission::rejected();
    }
    if (!ready() || !lock()) return OperationSubmission::busy();
    if (job_count_ + completion_count_locked() >= Capacity) {
        unlock();
        return OperationSubmission::busy();
    }

    JobSlot *free_job = nullptr;
    for (size_t i = 0; i < Capacity; ++i) {
        JobSlot &job = jobs_[i];
        if (!job.used) {
            free_job = &job;
            break;
        }
    }
    if (!free_job) {
        unlock();
        return OperationSubmission::busy();
    }

    const OperationTicket ticket =
        next_ticket_locked(command.generation);
    *free_job = {};
    free_job->used = true;
    free_job->ticket = ticket;
    free_job->operation = command.operation;
    free_job->sequence = ++next_sequence_;
    copy_cstr(free_job->source, sizeof(free_job->source),
              command.source.c_str());
    copy_cstr(free_job->destination, sizeof(free_job->destination),
              command.destination.c_str());
    job_count_++;
    unlock();

    wake();
    return OperationSubmission::accepted(ticket);
}

bool StoragePathService::abandon(OperationTicket ticket) {
    if (!ticket.valid() || !lock()) return false;

    JobSlot *job = find_job_locked(ticket);
    if (job) {
        if (job->started) {
            job->abandoned = true;
        } else {
            *job = {};
            if (job_count_ > 0) job_count_--;
        }
        unlock();
        return true;
    }

    CompletionSlot *completion = find_completion_locked(ticket);
    if (!completion) {
        unlock();
        return false;
    }
    *completion = {};
    unlock();
    return true;
}

bool StoragePathService::take_completion(
    OperationTicket ticket,
    StoragePathCompletion &completion) {
    if (!ticket.valid() || !lock()) return false;

    CompletionSlot *slot = find_completion_locked(ticket);
    if (!slot) {
        unlock();
        return false;
    }
    completion = slot->completion;
    *slot = {};
    unlock();
    return true;
}

StoragePathCompletion StoragePathService::execute_stat(const JobSlot &job) const {
    StoragePathCompletion completion;
    completion.ticket = job.ticket;

    Storage::Guard guard;
    if (!Storage::exists(job.source)) {
        completion.outcome = OperationOutcome::succeeded();
        return completion;
    }

    File node = Storage::open(job.source, "r");
    if (!node) {
        completion.outcome = OperationOutcome::failed();
        copy_cstr(completion.error, sizeof(completion.error),
                  "stat_open_failed");
        return completion;
    }

    completion.exists = true;
    completion.directory = node.isDirectory();
    completion.size = completion.directory ? 0 : node.size();
    completion.modified = node.getLastWrite();
    node.close();
    completion.outcome = OperationOutcome::succeeded();
    return completion;
}

StoragePathCompletion StoragePathService::execute_move_replacing(const JobSlot &job) const {
    StoragePathCompletion completion;
    completion.ticket = job.ticket;

    Storage::Guard guard;
    if (!Storage::exists(job.source)) {
        completion.outcome = OperationOutcome::failed();
        copy_cstr(completion.error, sizeof(completion.error),
                  "source_not_found");
        return completion;
    }
    if (Storage::exists(job.destination) &&
        !Storage::remove(job.destination)) {
        completion.outcome = OperationOutcome::failed();
        copy_cstr(completion.error, sizeof(completion.error),
                  "destination_remove_failed");
        return completion;
    }
    if (!Storage::rename(job.source, job.destination)) {
        completion.outcome = OperationOutcome::failed();
        copy_cstr(completion.error, sizeof(completion.error),
                  "move_failed");
        return completion;
    }

    completion.outcome = OperationOutcome::succeeded();
    return completion;
}

StoragePathCompletion StoragePathService::execute_remove(
    const JobSlot &job) const {
    StoragePathCompletion completion;
    completion.ticket = job.ticket;

    Storage::Guard guard;
    if (!Storage::exists(job.source)) {
        completion.outcome = OperationOutcome::succeeded();
        return completion;
    }
    if (!Storage::remove(job.source)) {
        completion.outcome = OperationOutcome::failed();
        copy_cstr(completion.error, sizeof(completion.error),
                  "remove_failed");
        return completion;
    }

    completion.outcome = OperationOutcome::succeeded();
    return completion;
}

StoragePathCompletion StoragePathService::execute(const JobSlot &job) const {
    if (!Storage::mounted()) {
        StoragePathCompletion completion;
        completion.ticket = job.ticket;
        completion.outcome = OperationOutcome::failed();
        copy_cstr(completion.error, sizeof(completion.error),
                  "storage_not_mounted");
        return completion;
    }
    if (job.operation == StoragePathOperation::MoveReplacing) {
        return execute_move_replacing(job);
    }
    if (job.operation == StoragePathOperation::Remove) {
        return execute_remove(job);
    }
    return execute_stat(job);
}

void StoragePathService::finish(
    OperationTicket ticket,
    const StoragePathCompletion &completion) {
    if (!lock_) return;
    xSemaphoreTake(lock_, portMAX_DELAY);

    JobSlot *job = find_job_locked(ticket);
    if (!job) {
        unlock();
        return;
    }
    const bool abandoned = job->abandoned;
    *job = {};
    if (job_count_ > 0) job_count_--;

    if (!abandoned) {
        CompletionSlot *slot = find_free_completion_locked();
        if (slot) {
            slot->used = true;
            slot->completion = completion;
        }
    }
    unlock();
}

bool StoragePathService::step() {
    if (!ready() || !lock(50)) return false;

    const size_t index = select_job_locked();
    if (index == SIZE_MAX) {
        unlock();
        return false;
    }
    jobs_[index].started = true;
    const JobSlot snapshot = jobs_[index];
    unlock();

    const StoragePathCompletion completion = execute(snapshot);
    finish(snapshot.ticket, completion);
    return true;
}

}  // namespace aircannect
