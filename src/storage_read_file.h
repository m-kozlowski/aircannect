#pragma once

#include "storage_internal.h"
#include "storage_path.h"

namespace aircannect {

// Storage-task-only file and metadata shared by successive prepared reads.
class StorageReadFile {
public:
    bool open(const char *path, uint32_t generation = 0);
    bool close(const char *path = nullptr);

    File file;
    uint64_t position = 0;
    uint64_t size = 0;
    uint64_t modified = 0;

private:
    char path_[AC_STORAGE_PATH_MAX] = {};
    uint32_t generation_ = 0;
};

}  // namespace aircannect
