#pragma once

#include <memory>
#include <stdint.h>
#include <string>

#include "large_byte_buffer.h"
#include "operation_outcome.h"
#include "storage_path.h"

namespace aircannect {

static constexpr size_t AC_STORAGE_UPLOAD_CHUNK_BYTES = 256 * 1024;
static constexpr uint64_t AC_STORAGE_UPLOAD_MAX_FILE_BYTES = 0xFFFFFFFFULL;

enum class StorageUploadConflict : uint8_t {
    Fail,
    Replace,
};

enum class StorageUploadState : uint8_t {
    Idle,
    Preparing,
    Ready,
    Writing,
    Paused,
    Publishing,
    Done,
    Cancelled,
    Error,
};

struct StorageUploadStartCommand {
    std::string path;
    std::string expected_sha256;
    uint64_t total_size = 0;
    uint64_t free_reserve_bytes = 0;
    StorageUploadConflict conflict = StorageUploadConflict::Fail;
    uint32_t generation = 0;

    bool valid() const {
        return generation != 0 &&
               total_size <= AC_STORAGE_UPLOAD_MAX_FILE_BYTES &&
               storage_user_path_valid(path.c_str()) && path != "/";
    }
};

struct StorageUploadStartResult {
    OperationAdmission admission = OperationAdmission::Rejected;
    uint32_t id = 0;
    size_t chunk_size = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool accepted() const {
        return admission == OperationAdmission::Accepted && id != 0;
    }
};

struct StorageUploadChunkCommand {
    uint32_t id = 0;
    uint64_t offset = 0;
    std::shared_ptr<const LargeByteBuffer> bytes;

    bool valid() const {
        return id != 0 && bytes && bytes->size() != 0 &&
               bytes->size() <= AC_STORAGE_UPLOAD_CHUNK_BYTES;
    }
};

struct StorageUploadChunkResult {
    OperationAdmission admission = OperationAdmission::Rejected;
    uint64_t committed_bytes = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool accepted() const {
        return admission == OperationAdmission::Accepted;
    }
};

struct StorageUploadStatus {
    uint32_t id = 0;
    StorageUploadState state = StorageUploadState::Idle;
    uint64_t total_bytes = 0;
    uint64_t committed_bytes = 0;
    char path[AC_STORAGE_PATH_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const {
        return state == StorageUploadState::Preparing ||
               state == StorageUploadState::Ready ||
               state == StorageUploadState::Writing ||
               state == StorageUploadState::Paused ||
               state == StorageUploadState::Publishing;
    }
};

const char *storage_upload_state_name(StorageUploadState state);

class StorageUploadPort {
public:
    virtual ~StorageUploadPort() = default;

    virtual StorageUploadStartResult start(
        const StorageUploadStartCommand &command) = 0;
    virtual StorageUploadChunkResult submit(
        const StorageUploadChunkCommand &command) = 0;
    virtual bool status(uint32_t id, StorageUploadStatus &status_out) const = 0;
    virtual bool cancel(uint32_t id) = 0;
};

}  // namespace aircannect
