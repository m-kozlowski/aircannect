#pragma once

#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "published_status_snapshot.h"
#include "storage_delete_port.h"

namespace aircannect {

class StorageDeleteService final : public StorageDeletePort {
public:
    using WakeCallback = void (*)();
    using ClaimMaintenanceCallback = bool (*)();
    using ReleaseMaintenanceCallback = void (*)();

    ~StorageDeleteService();

    // lifecycle and storage-task scheduling
    bool begin(WakeCallback wake,
               ClaimMaintenanceCallback claim_maintenance,
               ReleaseMaintenanceCallback release_maintenance);
    void set_task_available(bool available);
    void set_paused(bool paused);
    bool step();

    // delete requests
    bool start_selected(const char *base_path,
                        const char *const *names,
                        size_t count,
                        uint32_t *id_out = nullptr,
                        char *error_out = nullptr,
                        size_t error_out_size = 0) override;

    // status
    bool status(StorageDeleteStatus &out,
                uint32_t timeout_ms = 20) const override;
    bool active() const override { return active_.load(); }

private:
    struct WalkFrame;

    // lifecycle and locking
    bool allocate_owners();
    bool ready() const;
    void wake() const;
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    bool claim_maintenance_locked();
    void release_maintenance_locked();

    // status and request state
    void touch_status_locked();
    void set_error_locked(const char *error);
    bool append_root_locked(const char *path);
    bool validate_base_locked();

    // directory traversal
    void close_walk_locked();
    bool push_dir_locked(const char *path);
    bool ensure_dir_open_locked(WalkFrame &frame);
    bool delete_next_locked();
    bool delete_dir_step_locked();
    bool finish_done_locked();

    // synchronization and scheduling
    mutable SemaphoreHandle_t lock_ = nullptr;
    PublishedStatusSnapshot<StorageDeleteStatus> published_status_;
    WakeCallback wake_ = nullptr;
    ClaimMaintenanceCallback claim_maintenance_ = nullptr;
    ReleaseMaintenanceCallback release_maintenance_ = nullptr;
    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> pause_transition_pending_{false};
    std::atomic<bool> task_available_{false};
    bool maintenance_claimed_ = false;

    // request status
    StorageDeleteStatus status_;
    mutable bool status_dirty_ = false;
    uint32_t next_id_ = 1;
    bool base_checked_ = false;

    // requested roots
    uint32_t root_offsets_[AC_STORAGE_MAX_SELECTIONS] = {};
    char *path_bytes_ = nullptr;
    size_t path_bytes_len_ = 0;
    size_t path_bytes_capacity_ = 0;
    size_t current_root_ = 0;

    // directory traversal
    WalkFrame *walk_stack_ = nullptr;
    size_t walk_depth_ = 0;
    size_t walk_capacity_ = 0;
};

}  // namespace aircannect
