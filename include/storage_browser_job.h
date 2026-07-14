#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "background_worker.h"
#include "prepared_byte_ring.h"
#include "storage_path.h"

namespace aircannect {

struct StorageDirectoryEntryView {
    const char *name = nullptr;
    bool directory = false;
    uint64_t size = 0;
    uint64_t modified = 0;
};

class StorageDirectorySnapshot {
public:
    ~StorageDirectorySnapshot();

    const char *path() const { return path_; }
    size_t size() const { return entry_count_; }
    uint32_t revision() const { return revision_; }
    bool entry(size_t index, StorageDirectoryEntryView &out) const;

private:
    friend class StorageBrowserJob;

    struct Entry {
        uint64_t size = 0;
        uint64_t modified = 0;
        uint32_t name_offset = 0;
        bool directory = false;
    };

    char path_[AC_STORAGE_PATH_MAX] = {};
    Entry *entries_ = nullptr;
    Entry *sort_entries_ = nullptr;
    size_t entry_count_ = 0;
    size_t entry_capacity_ = 0;
    char *names_ = nullptr;
    size_t names_length_ = 0;
    size_t names_capacity_ = 0;
    uint32_t revision_ = 0;
};

enum class StorageListingRead : uint8_t {
    Ready,
    Preparing,
    Error,
};

struct StoragePreparedDownload;

enum class StorageDownloadPrepareState : uint8_t {
    Preparing,
    Ready,
    Busy,
    Error,
};

struct StorageDownloadPrepareStatus {
    StorageDownloadPrepareState state = StorageDownloadPrepareState::Preparing;
    uint32_t id = 0;
    uint64_t size = 0;
    char filename[AC_STORAGE_NAME_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
};

class StorageBrowserJob : public BackgroundJob {
public:
    // lifecycle and background service
    void begin();
    const char *name() const override { return "storage_browser"; }
    JobStep step() override;
    bool run_when_gate_closed(const char *reason) const override;
    bool drain_before_regular_jobs() const override { return true; }
    void on_preempt() override;

    // immutable directory snapshots
    StorageListingRead listing(
        const char *path,
        bool refresh,
        std::shared_ptr<const StorageDirectorySnapshot> &snapshot_out,
        char *error_out = nullptr,
        size_t error_out_size = 0);

    // prepared plain-file downloads
    StorageDownloadPrepareState prepare_download(
        const char *path,
        StorageDownloadPrepareStatus &status_out);
    bool begin_download(uint32_t id,
                        std::shared_ptr<StoragePreparedDownload> &download_out,
                        char *filename_out,
                        size_t filename_out_size,
                        uint64_t &size_out,
                        char *error_out = nullptr,
                        size_t error_out_size = 0);
    PreparedByteRead read_download(StoragePreparedDownload &download,
                                   uint8_t *buffer,
                                   size_t max_length,
                                   size_t offset);
    void finish_download(StoragePreparedDownload &download);

private:
    enum class ListingPhase : uint8_t {
        Idle,
        Scanning,
        Sorting,
    };

    // synchronization
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;

    // directory snapshots
    JobStep listing_step_locked();
    bool start_pending_listing_locked();
    bool ensure_listing_dir_open_locked();
    bool append_listing_entry_locked(const char *name,
                                     bool directory,
                                     uint64_t size,
                                     uint64_t modified);
    bool reserve_listing_entries_locked(size_t needed);
    bool reserve_listing_names_locked(size_t needed);
    JobStep sort_listing_step_locked();
    void publish_listing_locked();
    void fail_listing_locked(const char *error);
    void close_listing_dir_locked();
    void clear_listing_build_locked();
    int find_snapshot_locked(const char *path) const;
    void touch_snapshot_locked(size_t index);

    // plain downloads
    JobStep download_step();
    void fail_download_locked(StoragePreparedDownload &download,
                              const char *error);
    void close_download_file(StoragePreparedDownload &download);
    void retire_download_locked(StoragePreparedDownload &download,
                                bool success);

    mutable StaticSemaphore_t lock_storage_ = {};
    mutable SemaphoreHandle_t lock_ = nullptr;

    // published listing cache
    static constexpr size_t SNAPSHOT_SLOTS = 2;
    std::shared_ptr<const StorageDirectorySnapshot> snapshots_[SNAPSHOT_SLOTS];
    uint32_t snapshot_touches_[SNAPSHOT_SLOTS] = {};
    uint32_t snapshot_touch_counter_ = 0;
    uint32_t next_snapshot_revision_ = 1;

    // active/pending listing build
    ListingPhase listing_phase_ = ListingPhase::Idle;
    std::shared_ptr<StorageDirectorySnapshot> listing_build_;
    File listing_dir_;
    bool listing_dir_open_ = false;
    uint32_t listing_scanned_ = 0;
    size_t sort_width_ = 0;
    size_t sort_left_ = 0;
    size_t sort_mid_ = 0;
    size_t sort_right_ = 0;
    size_t sort_source_left_ = 0;
    size_t sort_source_right_ = 0;
    size_t sort_destination_ = 0;
    bool sort_source_is_entries_ = true;
    bool sort_merge_active_ = false;
    char pending_listing_path_[AC_STORAGE_PATH_MAX] = {};
    bool pending_listing_ = false;
    char listing_error_path_[AC_STORAGE_PATH_MAX] = {};
    char listing_error_[AC_STORAGE_ERROR_MAX] = {};

    // active plain-file producer
    std::shared_ptr<StoragePreparedDownload> active_download_;
    uint32_t next_download_id_ = 1;
};

}  // namespace aircannect
