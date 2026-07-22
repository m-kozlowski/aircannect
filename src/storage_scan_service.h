#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage_scan_port.h"

namespace aircannect {

class StorageScanService final : public StorageScanPort {
public:
    using WakeCallback = void (*)();
    using ClaimMaintenanceCallback = bool (*)();
    using ReleaseMaintenanceCallback = void (*)();

    ~StorageScanService();

    // lifecycle and storage-task scheduling
    bool begin(WakeCallback wake,
               ClaimMaintenanceCallback claim_maintenance,
               ReleaseMaintenanceCallback release_maintenance);
    void set_task_available(bool available);
    void set_paused(bool paused);
    bool step();

    // scan requests
    OperationSubmission request_scan(
        const StorageScanCommand &command) override;
    bool abandon(OperationTicket ticket) override;
    bool take_completion(
        OperationTicket ticket,
        StorageScanCompletion &completion) override;

private:
    struct Job;

    // lifecycle and locking
    bool ready() const;
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void wake() const;
    bool claim_maintenance_locked();
    void release_maintenance_locked();

    // scan execution
    bool start_next_root_locked(const char *&error);
    bool scan_next_entry_locked(const char *&error);
    bool push_directory_locked(const char *path,
                               bool recursive,
                               uint8_t root_index,
                               const char *&error);
    bool append_entry_locked(const char *path,
                             bool directory,
                             uint8_t root_index,
                             uint64_t size,
                             uint64_t modified);
    bool reserve_entries_locked(size_t needed);
    bool reserve_paths_locked(size_t needed);
    void close_directories_locked();

    // completion and job lifetime
    OperationTicket next_ticket_locked(uint32_t generation);
    void finish_locked(OperationOutcome outcome,
                       const char *error = nullptr);
    void clear_job_locked();

    mutable SemaphoreHandle_t lock_ = nullptr;
    WakeCallback wake_ = nullptr;
    ClaimMaintenanceCallback claim_maintenance_ = nullptr;
    ReleaseMaintenanceCallback release_maintenance_ = nullptr;
    std::atomic<bool> task_available_{false};
    std::atomic<bool> paused_{false};
    bool maintenance_claimed_ = false;

    Job *job_ = nullptr;
    bool completion_ready_ = false;
    StorageScanCompletion completion_;
    uint32_t next_ticket_id_ = 0;
};

}  // namespace aircannect
