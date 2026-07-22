#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "prepared_byte_ring.h"
#include "storage_path.h"

namespace aircannect {

class StorageDirectoryListing;

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

class StorageBrowserPort {
public:
    virtual ~StorageBrowserPort() = default;

    // immutable directory snapshots
    virtual StorageListingRead listing(
        const char *path,
        bool refresh,
        std::shared_ptr<const StorageDirectorySnapshot> &snapshot_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) = 0;

    // prepared plain-file downloads
    virtual StorageDownloadPrepareState prepare_download(
        const char *path,
        StorageDownloadPrepareStatus &status_out) = 0;
    virtual bool begin_download(
        uint32_t id,
        std::shared_ptr<StoragePreparedDownload> &download_out,
        char *filename_out,
        size_t filename_out_size,
        uint64_t &size_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) = 0;
    virtual PreparedByteRead read_download(StoragePreparedDownload &download,
                                           uint8_t *buffer,
                                           size_t max_length,
                                           size_t offset) = 0;
    virtual void finish_download(StoragePreparedDownload &download) = 0;
};

}  // namespace aircannect
