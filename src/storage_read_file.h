#pragma once

#include "storage_internal.h"
#include "storage_path.h"
#include <stdio.h>

namespace aircannect {

// Storage-task-only file and metadata shared by successive prepared reads.
class StorageReadFile {
public:
    StorageReadFile() = default;
    ~StorageReadFile();
    StorageReadFile(const StorageReadFile &) = delete;
    StorageReadFile &operator=(const StorageReadFile &) = delete;

    bool open(const char *path, uint32_t generation = 0,
              size_t whole_file_limit = 0);
    size_t read(uint8_t *data, size_t length);
    bool seek(uint32_t offset);
    bool close(const char *path = nullptr);

    uint64_t position = 0;
    uint64_t size = 0;
    uint64_t modified = 0;

private:
    FILE *file_ = nullptr;
    char path_[AC_STORAGE_PATH_MAX] = {};
    uint32_t generation_ = 0;
    bool has_modified_ = false;
};

}  // namespace aircannect
