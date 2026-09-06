#include "storage_internal.h"

#include <algorithm>
#include <string.h>

#include "memory_manager.h"
#include "storage_path.h"

namespace aircannect::Storage {

ParentDirectoryStep ensure_parent_directory_step(const char *path,
                                                 size_t &cursor) {
    if (!storage_user_path_valid(path) || cursor > strlen(path)) {
        return ParentDirectoryStep::Failed;
    }

    if (cursor == 0) {
        const char *last = strrchr(path, '/');
        if (last == path) return ParentDirectoryStep::Done;

        char parent[AC_STORAGE_PATH_MAX] = {};
        memcpy(parent, path, static_cast<size_t>(last - path));
        struct stat info {};
        if (file_stat(parent, info)) {
            const bool valid = S_ISDIR(info.st_mode);
            cursor = strlen(path);
            return valid ? ParentDirectoryStep::Done
                         : ParentDirectoryStep::Failed;
        }
    }

    const char *slash = strchr(path + (cursor ? cursor : 1), '/');
    if (!slash) return ParentDirectoryStep::Done;

    const size_t length = static_cast<size_t>(slash - path);
    char parent[AC_STORAGE_PATH_MAX] = {};
    memcpy(parent, path, length);
    if (!ensure_dir(parent)) return ParentDirectoryStep::Failed;

    cursor = length + 1;
    return ParentDirectoryStep::More;
}

bool ensure_parent_directories(const char *path) {
    size_t cursor = 0;
    ParentDirectoryStep result;
    do {
        result = ensure_parent_directory_step(path, cursor);
    } while (result == ParentDirectoryStep::More);

    return result == ParentDirectoryStep::Done;
}

uint64_t file_modified(const char *path) {
    struct stat info {};
    return file_stat(path, info) && S_ISREG(info.st_mode) && info.st_mtime > 0
        ? static_cast<uint64_t>(info.st_mtime) : 0;
}

size_t write_buffer(File &file, const uint8_t *data, size_t size) {
    // S3 SDMMC otherwise bounces PSRAM data through one 512-byte DMA sector
    // per transaction. Keep stdio buffers small so they do not undo staging.
    constexpr size_t DMA_BYTES = 4096;
    if (size < 1024) return file.write(data, size);

    auto *dma = static_cast<uint8_t *>(Memory::alloc_dma(DMA_BYTES));
    if (!dma) return file.write(data, size);

    size_t written = 0;
    while (written < size) {
        const size_t chunk = std::min(DMA_BYTES, size - written);
        memcpy(dma, data + written, chunk);
        const size_t part = file.write(dma, chunk);
        written += part;
        if (part != chunk) break;
    }

    Memory::free(dma);
    return written;
}

}  // namespace aircannect::Storage
