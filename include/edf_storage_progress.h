#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "storage_path.h"

namespace aircannect {

static constexpr size_t AC_EDF_STORAGE_PROGRESS_FILE_COUNT = 6;
static constexpr uint32_t AC_EDF_STORAGE_PROGRESS_NO_REWRITE_RECORD =
    UINT32_MAX;

struct EdfStorageProgressFile {
    bool open = false;
    char path[AC_STORAGE_PATH_MAX] = {};
    uint32_t request_id = 0;
    uint32_t record_count = 0;
    size_t header_size = 0;
    size_t record_size = 0;
    size_t byte_size = 0;
    std::shared_ptr<const LargeByteBuffer> header;

    // A rewrite revision is cumulative for the current path/request. The
    // sentinel means that no indexed record has been rewritten.
    uint64_t rewrite_revision = 0;
    uint32_t rewrite_min_record =
        AC_EDF_STORAGE_PROGRESS_NO_REWRITE_RECORD;
};

struct EdfStorageProgress {
    uint64_t revision = 0;
    EdfStorageProgressFile files[AC_EDF_STORAGE_PROGRESS_FILE_COUNT];
};

}  // namespace aircannect
