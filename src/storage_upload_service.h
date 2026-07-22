#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage_atomic_write_port.h"
#include "storage_upload_port.h"

namespace aircannect {

class StorageUploadService final : public StorageUploadPort {
public:
    using WakeCallback = void (*)();
    using ClaimMaintenanceCallback = bool (*)();
    using ReleaseMaintenanceCallback = void (*)();

    ~StorageUploadService();

    bool begin(WakeCallback wake,
               StorageAtomicWritePort &atomic_write_port,
               ClaimMaintenanceCallback claim_maintenance,
               ReleaseMaintenanceCallback release_maintenance);
    void set_task_available(bool available);
    void set_paused(bool paused);
    bool step();

    StorageUploadStartResult start(
        const StorageUploadStartCommand &command) override;
    StorageUploadChunkResult submit(
        const StorageUploadChunkCommand &command) override;
    bool status(uint32_t id, StorageUploadStatus &status_out) const override;
    bool cancel(uint32_t id) override;

private:
    struct Job;

    // Lifecycle and synchronization
    bool ready() const;
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void wake() const;
    uint32_t next_id_locked();
    bool claim_maintenance_locked();
    void release_maintenance_locked();

    // Receive and publication
    bool prepare_locked(const char *&error);
    bool pause_locked(const char *&error);
    bool resume_locked(const char *&error);
    bool validate_target_locked(const char *&error) const;
    bool temporary_size_matches_locked(uint64_t expected_size) const;
    bool write_locked(const char *&error);
    bool finalize_locked(const char *&error);
    bool submit_publication_locked(const char *&error);
    bool poll_publication_locked(const char *&error);
    bool cleanup_abandoned_step_locked();

    // Session lifetime
    void set_state_locked(StorageUploadState state);
    void finish_locked(StorageUploadState state,
                       const char *error = nullptr);
    void close_input_locked();
    void clear_job_locked();
    void cancel_locked();

    mutable SemaphoreHandle_t lock_ = nullptr;
    WakeCallback wake_ = nullptr;
    StorageAtomicWritePort *atomic_write_port_ = nullptr;
    ClaimMaintenanceCallback claim_maintenance_ = nullptr;
    ReleaseMaintenanceCallback release_maintenance_ = nullptr;
    std::atomic<bool> task_available_{false};
    std::atomic<bool> paused_{false};
    bool maintenance_claimed_ = false;

    Job *job_ = nullptr;
    uint32_t next_id_ = 0;
    uint32_t next_publish_generation_ = 0;
    bool cleanup_pending_ = true;
};

}  // namespace aircannect
