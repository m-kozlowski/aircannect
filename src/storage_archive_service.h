#pragma once

#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "published_status_snapshot.h"
#include "storage_archive_port.h"

namespace aircannect {

class StorageArchiveService final : public StorageArchivePort {
public:
    using WakeCallback = void (*)();
    using ClaimMaintenanceCallback = bool (*)();
    using ReleaseMaintenanceCallback = void (*)();

    ~StorageArchiveService();

    // lifecycle and storage-task scheduling
    bool begin(WakeCallback wake,
               ClaimMaintenanceCallback claim_maintenance,
               ReleaseMaintenanceCallback release_maintenance);
    void set_task_available(bool available);
    void set_paused(bool paused);
    bool step();

    // archive requests
    bool start(const char *path,
               bool recursive,
               uint32_t *id_out = nullptr,
               char *error_out = nullptr,
               size_t error_out_size = 0) override;
    bool start_selected(const char *base_path,
                        const char *const *names,
                        size_t count,
                        uint32_t *id_out = nullptr,
                        char *error_out = nullptr,
                        size_t error_out_size = 0) override;

    // status
    bool status(StorageArchiveStatus &out,
                uint32_t timeout_ms = 20) const override;
    bool active() const override { return active_.load(); }

    // download serving
    bool begin_download(
        uint32_t id,
        std::shared_ptr<StorageArchiveDownload> &download_out,
        char *filename_out,
        size_t filename_out_size,
        uint64_t &size_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) override;
    PreparedByteRead read_download(StorageArchiveDownload &download,
                                   uint8_t *buffer,
                                   size_t max_len,
                                   size_t offset) override;
    void finish_download(StorageArchiveDownload &download) override;

private:
    struct ArchiveEntry;
    struct WalkFrame;

    // lifecycle and locking
    bool ready() const;
    void wake() const;
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    bool claim_maintenance_locked();
    void release_maintenance_locked();

    // status and resource cleanup
    void touch_status_locked();
    void set_error_locked(const char *error);
    void reset_job_locked(bool keep_status);
    void close_walk_files_locked();
    void close_walk_locked();
    bool ensure_walk_stack_locked();
    void release_walk_stack_locked();
    void release_selection_locked();
    void release_build_buffers_locked();
    void apply_pause_locked();
    bool begin_job_locked(const char *source_path,
                          bool recursive,
                          const char *filename_base,
                          char *error_out,
                          size_t error_out_size);

    // metadata prewalk
    bool prepare_selection_step_locked();
    bool prepare_step_locked();
    bool append_entry_locked(const char *path,
                             uint64_t size,
                             bool directory,
                             time_t last_write);
    bool reserve_entries_locked(size_t needed);
    bool reserve_path_bytes_locked(size_t needed);
    bool push_walk_dir_locked(const char *path);
    bool ensure_walk_dir_open_locked(WalkFrame &frame);
    bool finalize_prepare_locked();

    // archive production and response streaming
    bool download_step_locked();
    size_t produce_download_locked(StorageArchiveDownload &download,
                                   uint8_t *buffer,
                                   size_t max_len);
    void close_download_input_locked(StorageArchiveDownload &download);
    void fail_download_locked(StorageArchiveDownload &download,
                              const char *error);

    // synchronization and scheduling
    mutable SemaphoreHandle_t lock_ = nullptr;
    PublishedStatusSnapshot<StorageArchiveStatus> published_status_;
    WakeCallback wake_ = nullptr;
    ClaimMaintenanceCallback claim_maintenance_ = nullptr;
    ReleaseMaintenanceCallback release_maintenance_ = nullptr;
    std::atomic<bool> active_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> pause_transition_pending_{false};
    std::atomic<uint32_t> download_progress_id_{0};
    std::atomic<uint32_t> download_bytes_sent_{0};
    std::atomic<bool> task_available_{false};
    bool maintenance_claimed_ = false;

    // archive status
    StorageArchiveStatus status_;
    mutable bool status_dirty_ = false;
    uint32_t next_id_ = 1;

    // archive metadata
    ArchiveEntry *entries_ = nullptr;
    size_t entry_count_ = 0;
    size_t entry_capacity_ = 0;
    char *path_bytes_ = nullptr;
    size_t path_bytes_len_ = 0;
    size_t path_bytes_capacity_ = 0;

    // selected-child staging
    char *selection_names_ = nullptr;
    size_t selection_count_ = 0;
    size_t selection_index_ = 0;
    bool selection_base_checked_ = false;

    // directory walk
    WalkFrame *walk_stack_ = nullptr;
    size_t walk_depth_ = 0;
    size_t walk_capacity_ = 0;

    // active prepared-byte producer
    std::shared_ptr<StorageArchiveDownload> active_download_;
};

}  // namespace aircannect
