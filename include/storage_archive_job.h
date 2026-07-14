#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "background_worker.h"
#include "prepared_byte_ring.h"
#include "published_status_snapshot.h"
#include "storage_path.h"

namespace aircannect {

struct StorageArchiveDownload;

static constexpr size_t AC_STORAGE_ARCHIVE_PATH_MAX = AC_STORAGE_PATH_MAX;
static constexpr size_t AC_STORAGE_ARCHIVE_NAME_MAX = AC_STORAGE_NAME_MAX;
static constexpr size_t AC_STORAGE_ARCHIVE_ERROR_MAX = AC_STORAGE_ERROR_MAX;
static constexpr size_t AC_STORAGE_ARCHIVE_MAX_SELECTIONS =
    AC_STORAGE_MAX_SELECTIONS;

enum class StorageArchiveState : uint8_t {
    Idle,
    Preparing,
    Ready,
    Downloading,
    Error,
};

const char *storage_archive_state_name(StorageArchiveState state);

struct StorageArchiveStatus {
    StorageArchiveState state = StorageArchiveState::Idle;
    uint32_t id = 0;
    bool recursive = true;
    bool psram_metadata = false;
    char source_path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
    char filename[AC_STORAGE_ARCHIVE_NAME_MAX] = {};
    char error[AC_STORAGE_ARCHIVE_ERROR_MAX] = {};
    uint32_t files = 0;
    uint32_t dirs = 0;
    uint32_t files_done = 0;
    uint64_t input_bytes = 0;
    uint64_t bytes_done = 0;
    uint64_t bytes_sent = 0;
    uint64_t archive_bytes = 0;
    uint64_t estimated_archive_bytes = 0;
    uint64_t free_bytes_at_start = 0;
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
};

class StorageArchiveJob : public BackgroundJob {
public:
    // lifecycle
    void begin();

    // background worker
    const char *name() const override { return "storage_archive"; }
    JobStep step() override;
    bool run_when_gate_closed(const char *reason) const override;
    JobStep step_when_gate_closed(const char *reason) override;
    bool drain_before_regular_jobs() const override { return true; }
    void on_preempt() override;

    // archive requests
    bool start(const char *path,
               bool recursive,
               uint32_t *id_out = nullptr,
               char *error_out = nullptr,
               size_t error_out_size = 0);
    bool start_selected(const char *base_path,
                        const char *const *names,
                        size_t count,
                        uint32_t *id_out = nullptr,
                        char *error_out = nullptr,
                        size_t error_out_size = 0);

    // status
    bool status(StorageArchiveStatus &out, uint32_t timeout_ms = 20) const;
    StorageArchiveStatus status() const;
    bool active() const;

    // download serving
    bool begin_download(uint32_t id,
                        std::shared_ptr<StorageArchiveDownload> &download_out,
                        char *filename_out,
                        size_t filename_out_size,
                        uint64_t &size_out,
                        char *error_out = nullptr,
                        size_t error_out_size = 0);
    PreparedByteRead read_download(StorageArchiveDownload &download,
                                   uint8_t *buffer,
                                   size_t max_len,
                                   size_t offset);
    void finish_download(StorageArchiveDownload &download);

private:
    struct ArchiveEntry;
    struct WalkFrame;

    // locking/status
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void touch_status_locked();
    void set_error_locked(const char *error);
    void reset_job_locked(bool keep_status);

    // resource cleanup
    void close_walk_files_locked();
    void close_walk_locked();
    bool ensure_walk_stack_locked();
    void release_walk_stack_locked();
    void release_selection_locked();
    void release_build_buffers_locked();
    void apply_preempt_locked();
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
    JobStep download_step_locked();
    size_t produce_download_locked(StorageArchiveDownload &download,
                                   uint8_t *buffer,
                                   size_t max_len);
    void close_download_input_locked(StorageArchiveDownload &download);
    void fail_download_locked(StorageArchiveDownload &download,
                              const char *error);

    // synchronization/status
    mutable SemaphoreHandle_t lock_ = nullptr;
    PublishedStatusSnapshot<StorageArchiveStatus> published_status_;
    std::atomic<bool> preempt_requested_{false};
    std::atomic<uint32_t> download_progress_id_{0};
    std::atomic<uint32_t> download_bytes_sent_{0};
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

bool storage_archive_valid_path(const char *path);

}  // namespace aircannect
