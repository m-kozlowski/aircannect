#pragma once

#include <atomic>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage_atomic_write_port.h"

namespace aircannect {

class StorageAtomicWriteService final : public StorageAtomicWritePort {
public:
    using WakeCallback = void (*)();

    ~StorageAtomicWriteService();

    bool begin(WakeCallback wake);
    void set_task_available(bool available);
    bool step(StorageAtomicWriteLane lane);

    OperationSubmission request_write(const StorageAtomicWriteCommand &command) override;
    bool abandon(OperationTicket ticket) override;
    bool take_completion(OperationTicket ticket, StorageAtomicWriteCompletion &completion) override;

private:
    enum class Phase : uint8_t {
        Open,
        Write,
        Flush,
        Record,
        Publish,
    };

    struct Job {
        bool active = false;
        bool abandoned = false;
        OperationTicket ticket;
        StorageAtomicWriteLane lane = StorageAtomicWriteLane::Maintenance;
        Phase phase = Phase::Open;
        std::shared_ptr<const LargeByteBuffer> bytes;
        File output;
        size_t offset = 0;
        uint64_t free_reserve_bytes = 0;
        char path[AC_STORAGE_PATH_MAX] = {};
    };

    // Service lifecycle
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void wake() const;
    bool ready() const;

    // Transaction recovery and execution
    bool recover_transaction_locked(const char *&error);
    bool open_locked(const char *&error);
    bool write_locked(const char *&error);
    bool flush_locked(const char *&error);
    bool record_locked(const char *&error);
    bool publish_locked(const char *&error);
    bool rollback_locked(const char *&error);

    // Completion and job lifetime
    void fail_locked(const char *error);
    void finish_locked(OperationOutcome outcome, const char *error = nullptr);
    void close_output_locked();
    void clear_job_locked();
    OperationTicket next_ticket_locked(uint32_t generation);

    mutable SemaphoreHandle_t lock_ = nullptr;
    WakeCallback wake_ = nullptr;
    std::atomic<bool> task_available_{false};

    bool recovery_needed_ = true;
    bool recovery_attempt_requested_ = true;
    Job *job_ = nullptr;

    bool completion_ready_ = false;
    StorageAtomicWriteCompletion completion_;
    uint32_t next_ticket_id_ = 0;
};

}  // namespace aircannect
