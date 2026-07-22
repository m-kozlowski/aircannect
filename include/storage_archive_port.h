#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "prepared_byte_ring.h"
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

class StorageArchivePort {
public:
    virtual ~StorageArchivePort() = default;

    // archive requests
    virtual bool start(const char *path,
                       bool recursive,
                       uint32_t *id_out = nullptr,
                       char *error_out = nullptr,
                       size_t error_out_size = 0) = 0;
    virtual bool start_selected(const char *base_path,
                                const char *const *names,
                                size_t count,
                                uint32_t *id_out = nullptr,
                                char *error_out = nullptr,
                                size_t error_out_size = 0) = 0;

    // status
    virtual bool status(StorageArchiveStatus &out,
                        uint32_t timeout_ms = 20) const = 0;
    virtual bool active() const = 0;

    // download serving
    virtual bool begin_download(
        uint32_t id,
        std::shared_ptr<StorageArchiveDownload> &download_out,
        char *filename_out,
        size_t filename_out_size,
        uint64_t &size_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) = 0;
    virtual PreparedByteRead read_download(StorageArchiveDownload &download,
                                           uint8_t *buffer,
                                           size_t max_len,
                                           size_t offset) = 0;
    virtual void finish_download(StorageArchiveDownload &download) = 0;
};

}  // namespace aircannect
