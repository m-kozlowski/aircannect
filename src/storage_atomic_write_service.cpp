#include "storage_atomic_write_service.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t WRITE_STEP_BYTES = 4096;
static constexpr uint32_t TRANSACTION_MAGIC = 0x52574341u;  // ACWR
static constexpr uint16_t TRANSACTION_VERSION = 1;

static constexpr const char *TRANSACTION_DIR =
    "/aircannect/.storage-transaction";
static constexpr const char *NEW_FILE_PATH =
    "/aircannect/.storage-transaction/new.bin";
static constexpr const char *PREVIOUS_FILE_PATH =
    "/aircannect/.storage-transaction/previous.bin";
static constexpr const char *RECORD_PATH =
    "/aircannect/.storage-transaction/replace.txn";
static constexpr const char *RECORD_TEMP_PATH =
    "/aircannect/.storage-transaction/replace.txn.tmp";

struct TransactionRecord {
    uint32_t magic = TRANSACTION_MAGIC;
    uint16_t version = TRANSACTION_VERSION;
    uint16_t path_length = 0;
    char path[AC_STORAGE_PATH_MAX] = {};
    uint32_t crc = 0;
};

static_assert(sizeof(TransactionRecord) == 204,
              "transaction record layout changed");

bool ensure_parent_directories(const char *path) {
    if (!storage_user_path_valid(path)) return false;

    char parent[AC_STORAGE_PATH_MAX] = {};
    copy_cstr(parent, sizeof(parent), path);
    char *last_slash = strrchr(parent, '/');
    if (!last_slash) return false;
    if (last_slash == parent) return true;
    *last_slash = '\0';

    for (char *cursor = parent + 1; *cursor; ++cursor) {
        if (*cursor != '/') continue;

        *cursor = '\0';
        const bool created = Storage::ensure_dir(parent);
        *cursor = '/';
        if (!created) return false;
    }
    return Storage::ensure_dir(parent);
}

bool reserved_transaction_path(const char *path) {
    if (!path) return false;

    const size_t prefix_length = strlen(TRANSACTION_DIR);
    return strncmp(path, TRANSACTION_DIR, prefix_length) == 0 &&
           (path[prefix_length] == '\0' || path[prefix_length] == '/');
}

void finalize_record(TransactionRecord &record) {
    record.path_length = static_cast<uint16_t>(strlen(record.path));
    record.crc = 0;
    record.crc = crc32_ieee(
        reinterpret_cast<const uint8_t *>(&record),
        offsetof(TransactionRecord, crc));
}

bool record_valid(const TransactionRecord &record) {
    if (record.magic != TRANSACTION_MAGIC ||
        record.version != TRANSACTION_VERSION ||
        record.path_length == 0 ||
        record.path_length >= sizeof(record.path) ||
        record.path[record.path_length] != '\0' ||
        strlen(record.path) != record.path_length ||
        !storage_user_path_valid(record.path) || record.path[1] == '\0' ||
        reserved_transaction_path(record.path)) {
        return false;
    }

    return record.crc == crc32_ieee(
                             reinterpret_cast<const uint8_t *>(&record),
                             offsetof(TransactionRecord, crc));
}

bool read_transaction_record(TransactionRecord &record) {
    File file = Storage::open(RECORD_PATH, "r");
    if (!file) return false;

    const size_t read = file.read(reinterpret_cast<uint8_t *>(&record),
                                  sizeof(record));
    const bool exact = read == sizeof(record) && !file.available();
    file.close();
    return exact && record_valid(record);
}

bool remove_transaction_artifacts() {
    return Storage::remove(NEW_FILE_PATH) &&
           Storage::remove(PREVIOUS_FILE_PATH) &&
           Storage::remove(RECORD_TEMP_PATH) && Storage::remove(RECORD_PATH);
}

}  // namespace

StorageAtomicWriteService::~StorageAtomicWriteService() {
    if (job_) {
        if (job_->output) job_->output.close();
        job_->~Job();
        Memory::free(job_);
    }
    if (lock_) vSemaphoreDelete(lock_);
}

bool StorageAtomicWriteService::begin(WakeCallback wake) {
    if (lock_) return job_ != nullptr;

    wake_ = wake;
    lock_ = xSemaphoreCreateMutex();
    void *memory = Memory::alloc_large(sizeof(Job), false);
    if (memory) job_ = new (memory) Job();
    if (lock_ && job_) return true;

    if (job_) {
        job_->~Job();
        Memory::free(job_);
        job_ = nullptr;
    }
    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }

    Log::logf(CAT_STORAGE, LOG_ERROR,
              "atomic write service unavailable\n");
    return false;
}

void StorageAtomicWriteService::set_task_available(bool available) {
    task_available_.store(available, std::memory_order_release);
    if (available) wake();
}

bool StorageAtomicWriteService::lock(uint32_t timeout_ms) const {
    return lock_ &&
           xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageAtomicWriteService::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StorageAtomicWriteService::wake() const {
    if (wake_) wake_();
}

bool StorageAtomicWriteService::ready() const {
    return lock_ && job_ &&
           task_available_.load(std::memory_order_acquire);
}

OperationTicket StorageAtomicWriteService::next_ticket_locked(uint32_t generation) {
    next_ticket_id_++;
    if (next_ticket_id_ == 0) next_ticket_id_++;
    return {next_ticket_id_, generation};
}

OperationSubmission StorageAtomicWriteService::request_write(
    const StorageAtomicWriteCommand &command) {
    if (!command.valid() || command.path.size() >= AC_STORAGE_PATH_MAX ||
        reserved_transaction_path(command.path.c_str())) {
        return OperationSubmission::rejected();
    }
    if (!ready() || !lock()) return OperationSubmission::busy();
    if (job_->active || completion_ready_) {
        unlock();
        return OperationSubmission::busy();
    }

    job_->active = true;
    job_->ticket = next_ticket_locked(command.generation);
    job_->lane = command.lane;
    job_->bytes = command.bytes;
    job_->free_reserve_bytes = command.free_reserve_bytes;
    copy_cstr(job_->path, sizeof(job_->path), command.path.c_str());
    recovery_attempt_requested_ = true;
    const OperationTicket ticket = job_->ticket;
    unlock();

    wake();
    return OperationSubmission::accepted(ticket);
}

bool StorageAtomicWriteService::abandon(OperationTicket ticket) {
    if (!ticket.valid() || !lock(50)) return false;

    if (job_->active && job_->ticket == ticket) {
        job_->abandoned = true;
        unlock();
        wake();
        return true;
    }
    if (completion_ready_ && completion_.ticket == ticket) {
        completion_ready_ = false;
        completion_ = {};
        unlock();
        return true;
    }

    unlock();
    return false;
}

bool StorageAtomicWriteService::take_completion(OperationTicket ticket,
                                                StorageAtomicWriteCompletion &completion) {
    if (!ticket.valid() || !lock()) return false;
    if (!completion_ready_ || completion_.ticket != ticket) {
        unlock();
        return false;
    }

    completion = completion_;
    completion_ = {};
    completion_ready_ = false;
    unlock();
    return true;
}

bool StorageAtomicWriteService::recover_transaction_locked(
    const char *&error) {
    Storage::Guard guard;

    if (!Storage::exists(TRANSACTION_DIR)) {
        recovery_needed_ = false;
        return true;
    }

    const bool has_record = Storage::exists(RECORD_PATH);
    const bool has_previous = Storage::exists(PREVIOUS_FILE_PATH);
    if (!has_record) {
        if (has_previous) {
            error = "orphaned_previous_file";
            return false;
        }
        if (!remove_transaction_artifacts()) {
            error = "orphan_cleanup_failed";
            return false;
        }

        recovery_needed_ = false;
        return true;
    }

    TransactionRecord record;
    if (!read_transaction_record(record)) {
        if (has_previous) {
            error = "transaction_record_invalid";
            return false;
        }
        if (!remove_transaction_artifacts()) {
            error = "invalid_transaction_cleanup_failed";
            return false;
        }

        recovery_needed_ = false;
        return true;
    }

    const bool target_exists = Storage::exists(record.path);
    if (!target_exists && has_previous &&
        !Storage::rename(PREVIOUS_FILE_PATH, record.path)) {
        error = "previous_restore_failed";
        return false;
    }
    if (!remove_transaction_artifacts()) {
        error = "transaction_cleanup_failed";
        return false;
    }

    recovery_needed_ = false;
    return true;
}

bool StorageAtomicWriteService::open_locked(const char *&error) {
    const StorageStatus storage = Storage::status();
    if (!storage.mounted) {
        error = "storage_not_mounted";
        return false;
    }

    const uint64_t length = job_->bytes->size();
    if (job_->free_reserve_bytes > UINT64_MAX - length ||
        storage.free_bytes < length + job_->free_reserve_bytes) {
        error = "not_enough_storage";
        return false;
    }

    Storage::Guard guard;
    if (!ensure_parent_directories(job_->path) ||
        !Storage::ensure_dir(TRANSACTION_DIR)) {
        error = "parent_create_failed";
        return false;
    }
    if (!remove_transaction_artifacts()) {
        error = "transaction_cleanup_failed";
        return false;
    }

    job_->output = Storage::open(NEW_FILE_PATH, "w");
    if (!job_->output) {
        error = "temporary_open_failed";
        return false;
    }
    (void)job_->output.setBufferSize(512);
    job_->phase = Phase::Write;
    return true;
}

bool StorageAtomicWriteService::write_locked(const char *&error) {
    if (!job_->output || !job_->bytes || job_->offset >= job_->bytes->size()) {
        error = "write_state_invalid";
        return false;
    }

    const size_t remaining = job_->bytes->size() - job_->offset;
    const size_t wanted = std::min(remaining, WRITE_STEP_BYTES);
    size_t written = 0;
    {
        Storage::Guard guard;
        written = job_->output.write(job_->bytes->data() + job_->offset,
                                     wanted);
    }
    if (written != wanted) {
        error = "write_failed";
        return false;
    }

    job_->offset += written;
    if (job_->offset == job_->bytes->size()) job_->phase = Phase::Flush;
    return true;
}

void StorageAtomicWriteService::close_output_locked() {
    if (!job_->output) return;

    Storage::Guard guard;
    job_->output.close();
}

bool StorageAtomicWriteService::flush_locked(const char *&error) {
    if (!job_->output) {
        error = "flush_state_invalid";
        return false;
    }

    {
        Storage::Guard guard;
        job_->output.flush();
        job_->output.close();
    }
    job_->phase = Phase::Record;
    return true;
}

bool StorageAtomicWriteService::record_locked(const char *&error) {
    TransactionRecord record;
    copy_cstr(record.path, sizeof(record.path), job_->path);
    finalize_record(record);

    Storage::Guard guard;
    File file = Storage::open(RECORD_TEMP_PATH, "w");
    if (!file) {
        error = "transaction_record_open_failed";
        return false;
    }

    const size_t written = file.write(
        reinterpret_cast<const uint8_t *>(&record), sizeof(record));
    file.flush();
    file.close();
    if (written != sizeof(record)) {
        error = "transaction_record_write_failed";
        return false;
    }
    if (!Storage::rename(RECORD_TEMP_PATH, RECORD_PATH)) {
        error = "transaction_record_publish_failed";
        return false;
    }

    job_->phase = Phase::Publish;
    return true;
}

bool StorageAtomicWriteService::publish_locked(const char *&error) {
    Storage::Guard guard;

    const bool had_previous = Storage::exists(job_->path);
    if (had_previous && !Storage::rename(job_->path, PREVIOUS_FILE_PATH)) {
        error = "previous_preserve_failed";
        return false;
    }
    if (!Storage::rename(NEW_FILE_PATH, job_->path)) {
        if (had_previous) {
            (void)Storage::rename(PREVIOUS_FILE_PATH, job_->path);
        }
        error = "publish_failed";
        return false;
    }

    if (!Storage::remove(PREVIOUS_FILE_PATH) ||
        !Storage::remove(RECORD_PATH)) {
        recovery_needed_ = true;
        recovery_attempt_requested_ = true;
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "atomic write published with pending cleanup path=%s\n",
                  job_->path);
    }
    return true;
}

bool StorageAtomicWriteService::rollback_locked(const char *&error) {
    recovery_needed_ = true;
    recovery_attempt_requested_ = true;
    return recover_transaction_locked(error);
}

void StorageAtomicWriteService::clear_job_locked() {
    close_output_locked();
    job_->~Job();
    new (job_) Job();
}

void StorageAtomicWriteService::finish_locked(OperationOutcome outcome,
                                              const char *error) {
    const bool abandoned = job_->abandoned;
    const OperationTicket ticket = job_->ticket;
    const uint64_t bytes_written = job_->offset;

    if (!abandoned) {
        completion_ = {};
        completion_.ticket = ticket;
        completion_.outcome = outcome;
        completion_.bytes_written = bytes_written;
        copy_cstr(completion_.error, sizeof(completion_.error),
                  error ? error : "");
        completion_ready_ = true;
    }
    clear_job_locked();
}

void StorageAtomicWriteService::fail_locked(const char *error) {
    close_output_locked();

    const char *rollback_error = nullptr;
    if (!rollback_locked(rollback_error) && rollback_error) {
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "atomic write rollback failed error=%s\n",
                  rollback_error);
    }
    finish_locked(OperationOutcome::failed(), error);
}

bool StorageAtomicWriteService::step(StorageAtomicWriteLane lane) {
    if (!ready() || !lock(50)) return false;

    if (lane == StorageAtomicWriteLane::Foreground && recovery_needed_ &&
        recovery_attempt_requested_) {
        recovery_attempt_requested_ = false;
        const char *recovery_error = nullptr;
        const bool recovered = recover_transaction_locked(recovery_error);
        if (!recovered) {
            Log::logf(CAT_STORAGE, LOG_ERROR,
                      "atomic write recovery failed error=%s\n",
                      recovery_error ? recovery_error : "unknown");
            if (job_->active) {
                finish_locked(OperationOutcome::failed(),
                              recovery_error ? recovery_error
                                             : "recovery_failed");
            }
        }
        unlock();
        return true;
    }

    if (!job_->active || job_->lane != lane) {
        unlock();
        return false;
    }

    if (job_->abandoned) {
        close_output_locked();
        const char *rollback_error = nullptr;
        if (!rollback_locked(rollback_error) && rollback_error) {
            Log::logf(CAT_STORAGE, LOG_ERROR,
                      "atomic write cancel rollback failed error=%s\n",
                      rollback_error);
        }
        finish_locked(OperationOutcome::cancelled());
        unlock();
        return true;
    }

    const char *error = nullptr;
    bool ok = false;
    switch (job_->phase) {
        case Phase::Open:
            ok = open_locked(error);
            break;
        case Phase::Write:
            ok = write_locked(error);
            break;
        case Phase::Flush:
            ok = flush_locked(error);
            break;
        case Phase::Record:
            ok = record_locked(error);
            break;
        case Phase::Publish:
            ok = publish_locked(error);
            if (ok) finish_locked(OperationOutcome::succeeded());
            break;
    }

    if (!ok) fail_locked(error ? error : "write_failed");
    unlock();
    return true;
}

}  // namespace aircannect
