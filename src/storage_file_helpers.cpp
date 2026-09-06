#include "storage_internal.h"

#include <algorithm>
#include <string.h>
#include <unistd.h>

#include "memory_manager.h"
#include "large_object.h"
#include "string_util.h"
#include "storage_path.h"
#include "storage_range_write_port.h"

namespace aircannect::Storage {

namespace {

struct WriteHandleCache {
    struct Entry {
        char path[AC_STORAGE_PATH_MAX] = {};
        int descriptor = -1;
    };

    static constexpr size_t Capacity = 3;
    Entry entries[Capacity];
    size_t count = 0;

    int take(size_t index) {
        const int descriptor = entries[index].descriptor;
        for (size_t i = index + 1; i < count; ++i) entries[i - 1] = entries[i];
        entries[--count] = {};
        return descriptor;
    }
};

WriteHandleCache *write_handles = nullptr;
bool write_close_failed = false;

void close_descriptor(int descriptor) {
    if (::close(descriptor) != 0) write_close_failed = true;
}

}  // namespace

int take_write_handle(const char *path) {
    if (!write_handles) return -1;

    for (size_t i = 0; i < write_handles->count; ++i) {
        if (strcmp(write_handles->entries[i].path, path) == 0) {
            return write_handles->take(i);
        }
    }

    if (write_handles->count == WriteHandleCache::Capacity) {
        close_descriptor(write_handles->take(0));
    }
    return -1;
}

void close_write_handle(const char *path, int descriptor, bool retain) {
    if (descriptor < 0) return;
    if (retain && !write_handles) {
        write_handles = LargeObject::create<WriteHandleCache>();
    }

    if (!retain || !write_handles) {
        close_descriptor(descriptor);
        return;
    }

    auto &entry = write_handles->entries[write_handles->count++];
    copy_cstr(entry.path, sizeof(entry.path), path);
    entry.descriptor = descriptor;
}

bool release_write_handles() {
    if (!write_handles) return false;

    const bool released = write_handles->count != 0;
    while (write_handles->count) close_descriptor(write_handles->take(0));
    LargeObject::destroy(write_handles);
    write_handles = nullptr;
    return released;
}

bool finish_write_handles() {
    release_write_handles();
    const bool succeeded = !write_close_failed;
    write_close_failed = false;
    return succeeded;
}

void release_write_handle(const char *path) {
    if (!write_handles) return;
    for (size_t i = 0; i < write_handles->count; ++i) {
        if (strcmp(write_handles->entries[i].path, path) == 0) {
            close_descriptor(write_handles->take(i));
            return;
        }
    }
}

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

namespace {

template <typename Write, typename Span>
size_t write_staged(Write write, Span span, size_t size) {
    // S3 SDMMC otherwise bounces PSRAM data through one 512-byte DMA sector
    // per transaction. Keep stdio buffers small so they do not undo staging.
    constexpr size_t DMA_BYTES = 4096;
    auto *dma = size >= 1024
        ? static_cast<uint8_t *>(Memory::alloc_dma(DMA_BYTES)) : nullptr;

    size_t written = 0;
    while (written < size) {
        size_t available = 0;
        const uint8_t *data = span(written, available);
        if (!data || !available) break;

        size_t chunk = std::min(available, size - written);
        if (dma) {
            chunk = std::min(DMA_BYTES, size - written);
            size_t copied = 0;
            while (copied < chunk) {
                data = span(written + copied, available);
                if (!data || !available) break;
                const size_t count = std::min(available, chunk - copied);
                memcpy(dma + copied, data, count);
                copied += count;
            }
            if (copied != chunk) break;
            data = dma;
        }
        const size_t part = write(data, chunk);
        written += part;
        if (part != chunk) break;
    }

    Memory::free(dma);
    return written;
}

}  // namespace

size_t write_buffer(File &file, const uint8_t *data, size_t size) {
    return write_staged([&file](const uint8_t *bytes, size_t count) {
        return file.write(bytes, count);
    }, [data, size](size_t offset, size_t &available) {
        available = size - offset;
        return data + offset;
    }, size);
}

size_t write_buffers(int descriptor, const StorageRangeWriteCommand &command,
                     size_t offset, size_t size) {
    return write_staged([descriptor](const uint8_t *bytes, size_t count) {
        const ssize_t written = ::write(descriptor, bytes, count);
        return written > 0 ? static_cast<size_t>(written) : 0;
    }, [&command, offset](size_t position, size_t &available) {
        return command.span(offset + position, available);
    }, size);
}

}  // namespace aircannect::Storage
