#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "background_worker.h"

namespace aircannect {

static constexpr size_t AC_STORAGE_ARCHIVE_PATH_MAX = 192;
static constexpr size_t AC_STORAGE_ARCHIVE_NAME_MAX = 80;
static constexpr size_t AC_STORAGE_ARCHIVE_ERROR_MAX = 64;
static constexpr const char *AC_STORAGE_ARCHIVE_TEMP_DIR =
    "/aircannect/tmp/archive";

enum class StorageArchiveState : uint8_t {
    Idle,
    Preparing,
    Building,
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
    char archive_path[AC_STORAGE_ARCHIVE_PATH_MAX] = {};
    char filename[AC_STORAGE_ARCHIVE_NAME_MAX] = {};
    char error[AC_STORAGE_ARCHIVE_ERROR_MAX] = {};
    uint32_t files = 0;
    uint32_t dirs = 0;
    uint32_t files_done = 0;
    uint64_t input_bytes = 0;
    uint64_t bytes_done = 0;
    uint64_t archive_bytes = 0;
    uint64_t estimated_archive_bytes = 0;
    uint64_t free_bytes_at_start = 0;
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
};

class StorageArchiveJob : public BackgroundJob {
public:
    void begin();

    const char *name() const override { return "storage_archive"; }
    JobStep step() override;
    void on_preempt() override;

    bool start(const char *path,
               bool recursive,
               uint32_t *id_out = nullptr,
               char *error_out = nullptr,
               size_t error_out_size = 0);
    StorageArchiveStatus status() const;

    bool download_info(uint32_t id,
                       char *path_out,
                       size_t path_out_size,
                       char *filename_out,
                       size_t filename_out_size,
                       uint64_t &size_out) const;
    bool mark_download_started(uint32_t id,
                               char *error_out = nullptr,
                               size_t error_out_size = 0);
    void mark_download_finished(uint32_t id);

private:
    struct ArchiveEntry;
    struct WalkFrame;

    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void set_error_locked(const char *error);
    void reset_job_locked(bool keep_status);
    void close_active_files_locked();
    void close_walk_locked();
    void apply_preempt_locked();
    bool cleanup_ready_archive_locked();
    void cleanup_stale_temp_locked();

    bool prepare_step_locked();
    bool build_step_locked();
    bool ensure_temp_dirs_locked();
    bool open_output_locked();
    bool append_entry_locked(const char *path, uint64_t size, bool directory);
    bool reserve_entries_locked(size_t needed);
    bool reserve_path_bytes_locked(size_t needed);
    bool push_walk_dir_locked(const char *path);
    bool ensure_walk_dir_open_locked(WalkFrame &frame);
    bool finalize_prepare_locked();
    bool begin_current_file_locked();
    bool copy_current_file_step_locked();
    bool finish_current_file_locked();
    bool write_central_step_locked();
    bool write_eocd_locked();

    mutable SemaphoreHandle_t lock_ = nullptr;
    std::atomic<bool> preempt_requested_{false};
    StorageArchiveStatus status_;
    uint32_t next_id_ = 1;

    ArchiveEntry *entries_ = nullptr;
    size_t entry_count_ = 0;
    size_t entry_capacity_ = 0;
    char *path_bytes_ = nullptr;
    size_t path_bytes_len_ = 0;
    size_t path_bytes_capacity_ = 0;
    uint8_t *io_buffer_ = nullptr;

    WalkFrame *walk_stack_ = nullptr;
    size_t walk_depth_ = 0;
    size_t walk_capacity_ = 0;

    File output_;
    File input_;
    bool output_open_ = false;
    bool input_open_ = false;
    bool current_file_active_ = false;
    size_t current_file_index_ = 0;
    uint64_t current_file_offset_ = 0;
    uint32_t current_crc_ = 0;
    size_t central_index_ = 0;
    uint64_t central_start_offset_ = 0;
    uint64_t central_size_ = 0;
    bool central_started_ = false;
    uint32_t cleanup_due_ms_ = 0;
    uint32_t stale_cleanup_due_ms_ = 1;
};

bool storage_archive_valid_path(const char *path);

}  // namespace aircannect
