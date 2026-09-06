#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage_range_write_port.h"

namespace aircannect {

class StorageRangeWriteService final : public StorageRangeWritePort {
public:
    using WakeCallback = void (*)();

    ~StorageRangeWriteService();
    bool begin(WakeCallback wake);
    void set_task_available(bool available);
    void set_retention_allowed(bool allowed);

    // Called only by the StorageService task, below EDF work.
    bool step(StorageAtomicWriteLane lane);

    OperationSubmission request_write(
        const StorageRangeWriteCommand &command) override;
    bool abandon(OperationTicket ticket) override;
    void release_handles() override;
    bool take_completion(
        OperationTicket ticket,
        StorageRangeWriteCompletion &completion) override;

private:
    enum class Phase : uint8_t { Open, Write, Flush };

    struct Job {
        StorageRangeWriteCommand command;
        OperationTicket ticket;
        int output = -1;
        size_t written = 0;
        size_t parent_cursor = 0;
        Phase phase = Phase::Open;
        bool abandoned = false;
    };

    // Lifecycle and cross-task admission
    bool ready() const;
    bool lock() const;
    void unlock() const;
    void wake() const;
    bool apply_abandon_locked();

    // Owner-task execution and completion
    const char *open_locked();
    const char *write_locked();
    void finish_locked(OperationOutcome outcome, const char *error = nullptr);

    SemaphoreHandle_t mutex_ = nullptr;
    WakeCallback wake_ = nullptr;
    std::atomic<bool> task_available_{false};
    std::atomic<uint64_t> abandon_request_{0};
    std::atomic<bool> release_requested_{false};
    std::atomic<bool> retention_allowed_{true};
    uint32_t next_ticket_id_ = 0;

    Job *job_ = nullptr;
    StorageRangeWriteCompletion completion_;
};

}  // namespace aircannect
