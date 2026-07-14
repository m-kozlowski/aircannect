#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "background_worker.h"
#include "storage_path.h"

namespace aircannect {

enum class StorageDeleteState : uint8_t {
    Idle,
    Deleting,
    Done,
    Error,
};

const char *storage_delete_state_name(StorageDeleteState state);

struct StorageDeleteStatus {
    StorageDeleteState state = StorageDeleteState::Idle;
    uint32_t id = 0;
    char base_path[AC_STORAGE_PATH_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint32_t roots = 0;
    uint32_t roots_done = 0;
    uint32_t files_deleted = 0;
    uint32_t dirs_deleted = 0;
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
};

class StorageDeleteJob : public BackgroundJob {
public:
    // lifecycle
    void begin();

    // background worker
    const char *name() const override { return "storage_delete"; }
    JobStep step() override;
    void on_preempt() override;

    // delete requests
    bool start_selected(const char *base_path,
                        const char *const *names,
                        size_t count,
                        uint32_t *id_out = nullptr,
                        char *error_out = nullptr,
                        size_t error_out_size = 0);

    // status
    bool status(StorageDeleteStatus &out,
                uint32_t timeout_ms = 1000) const;
    StorageDeleteStatus status() const;
    bool active() const;

private:
    struct WalkFrame;

    // locking/status
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void set_error_locked(const char *error);
    void reset_job_locked(bool keep_status);

    // resource cleanup
    void release_path_metadata_locked();
    void close_walk_locked();
    bool ensure_walk_stack_locked();
    void release_walk_stack_locked();
    void apply_preempt_locked();
    bool reserve_path_bytes_locked(size_t needed);
    bool append_root_locked(const char *path);
    bool push_dir_locked(const char *path);
    bool ensure_dir_open_locked(WalkFrame &frame);
    bool delete_next_locked();
    bool delete_dir_step_locked();
    bool finish_done_locked();

    // synchronization/status
    mutable SemaphoreHandle_t lock_ = nullptr;
    std::atomic<bool> preempt_requested_{false};
    StorageDeleteStatus status_;
    uint32_t next_id_ = 1;

    // requested roots
    uint32_t root_offsets_[AC_STORAGE_MAX_SELECTIONS] = {};
    char *path_bytes_ = nullptr;
    size_t path_bytes_len_ = 0;
    size_t path_bytes_capacity_ = 0;
    size_t current_root_ = 0;

    // directory walk
    WalkFrame *walk_stack_ = nullptr;
    size_t walk_depth_ = 0;
    size_t walk_capacity_ = 0;
};

}  // namespace aircannect
