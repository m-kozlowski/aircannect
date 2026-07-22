#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage_path_port.h"

namespace aircannect {

class StoragePathService final : public StoragePathPort {
public:
    using WakeCallback = void (*)();

    ~StoragePathService();

    bool begin(WakeCallback wake);
    void set_task_available(bool available);
    bool step();

    OperationSubmission request(const StoragePathCommand &command) override;
    bool abandon(OperationTicket ticket) override;
    bool take_completion(
        OperationTicket ticket,
        StoragePathCompletion &completion) override;

private:
    static constexpr size_t Capacity = 2;

    struct JobSlot {
        bool used = false;
        bool started = false;
        bool abandoned = false;
        OperationTicket ticket;
        StoragePathOperation operation = StoragePathOperation::Stat;
        uint64_t sequence = 0;
        char source[AC_STORAGE_PATH_MAX] = {};
        char destination[AC_STORAGE_PATH_MAX] = {};
    };

    struct CompletionSlot {
        bool used = false;
        StoragePathCompletion completion;
    };

    bool ready() const;
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void wake() const;

    JobSlot *find_job_locked(OperationTicket ticket);
    CompletionSlot *find_completion_locked(OperationTicket ticket);
    CompletionSlot *find_free_completion_locked();
    size_t select_job_locked() const;
    size_t completion_count_locked() const;
    OperationTicket next_ticket_locked(uint32_t generation);

    StoragePathCompletion execute(const JobSlot &job) const;
    StoragePathCompletion execute_stat(const JobSlot &job) const;
    StoragePathCompletion execute_move_replacing(const JobSlot &job) const;
    void finish(OperationTicket ticket,
                const StoragePathCompletion &completion);

    mutable SemaphoreHandle_t lock_ = nullptr;
    WakeCallback wake_ = nullptr;
    bool task_available_ = false;
    JobSlot *jobs_ = nullptr;
    CompletionSlot *completions_ = nullptr;
    size_t job_count_ = 0;
    uint32_t next_ticket_id_ = 0;
    uint64_t next_sequence_ = 0;
};

}  // namespace aircannect
