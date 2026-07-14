#pragma once

#include <Arduino.h>
#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "background_worker.h"
#include "prepared_byte_ring.h"
#include "storage_path.h"

namespace aircannect {

class StorageDirectoryListing;
class StorageDownloadProducer;

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
    friend class StorageDirectoryListing;

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
    ~StorageBrowserJob();
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
    bool ensure_owners();

    StorageDirectoryListing *listing_ = nullptr;
    StorageDownloadProducer *download_ = nullptr;
};

}  // namespace aircannect
