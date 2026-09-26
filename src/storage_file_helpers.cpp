#include "storage_internal.h"

#include <algorithm>
#include <errno.h>
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

void close_cached(size_t index) {
    auto &entry = write_handles->entries[index];
    if (close_descriptor(entry.path, entry.descriptor) != 0) {
        write_close_failed = true;
    }
    (void)write_handles->take(index);
}

}  // namespace

int close_descriptor(const char *path, int descriptor) {
    const int result = ::close(descriptor);
    if (result != 0) log_io_error("close", path, errno);
    return result;
}

int take_write_handle(const char *path) {
    if (!write_handles) return -1;

    for (size_t i = 0; i < write_handles->count; ++i) {
        if (strcmp(write_handles->entries[i].path, path) == 0) {
            return write_handles->take(i);
        }
    }

    if (write_handles->count == WriteHandleCache::Capacity) {
        close_cached(0);
    }
    return -1;
}

void close_write_handle(const char *path, int descriptor, bool retain) {
    if (descriptor < 0) return;
    if (retain && !write_handles) {
        write_handles = LargeObject::create<WriteHandleCache>();
    }

    if (!retain || !write_handles) {
        if (close_descriptor(path, descriptor) != 0) write_close_failed = true;
        return;
    }

    auto &entry = write_handles->entries[write_handles->count++];
    copy_cstr(entry.path, sizeof(entry.path), path);
    entry.descriptor = descriptor;
}

bool release_write_handles() {
    if (!write_handles) return false;

    const bool released = write_handles->count != 0;
    while (write_handles->count) close_cached(0);
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
            close_cached(i);
            return;
        }
    }
}

ParentDirectoryStep ensure_parent_directory_step(const char *path,
                                                 ParentDirectoryCursor &cursor) {
    if (!storage_user_path_valid(path) || cursor.offset > strlen(path)) {
        return ParentDirectoryStep::Failed;
    }

    const size_t length = strlen(path);
    if (cursor.offset == length) return ParentDirectoryStep::Done;

    if (cursor.offset == 0) {
        const char *last = strrchr(path, '/');
        if (last == path) {
            cursor.offset = length;
            return ParentDirectoryStep::Done;
        }

        cursor.offset = static_cast<size_t>(last - path);
    }

    char parent[AC_STORAGE_PATH_MAX] = {};
    memcpy(parent, path, cursor.offset);

    if (!cursor.creating) {
        struct stat info {};
        if (!file_stat(parent, info)) {
            if (errno != ENOENT) return ParentDirectoryStep::Failed;

            // Search upward one parent per turn; the mounted root already exists.
            const size_t previous =
                static_cast<size_t>(strrchr(parent, '/') - parent);

            if (previous == 0) cursor.creating = true;
            else cursor.offset = previous;
            return ParentDirectoryStep::More;
        }

        if (!S_ISDIR(info.st_mode)) return ParentDirectoryStep::Failed;
        cursor.creating = true;
    } else if (!ensure_dir(parent)) {
        return ParentDirectoryStep::Failed;
    }

    const char *next = strchr(path + cursor.offset + 1, '/');
    cursor.offset = next ? static_cast<size_t>(next - path) : length;
    return next ? ParentDirectoryStep::More : ParentDirectoryStep::Done;
}

bool ensure_parent_directories(const char *path) {
    ParentDirectoryCursor cursor;
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

WriteStagingBuffer::~WriteStagingBuffer() {
    reset();
}

uint8_t *WriteStagingBuffer::get(size_t write_size) {
    if (!attempted_ && write_size >= 1024) {
        attempted_ = true;
        data_ = static_cast<uint8_t *>(Memory::alloc_dma(Capacity));
    }

    return data_;
}

void WriteStagingBuffer::reset() {
    Memory::free(data_);
    data_ = nullptr;
    attempted_ = false;
}

namespace {

template <typename Write, typename Span>
size_t write_staged(Write write, Span span, size_t size,
                    WriteStagingBuffer &staging) {
    // S3 SDMMC otherwise bounces PSRAM data through one 512-byte DMA sector
    // per transaction. Keep stdio buffers small so they do not undo staging.
    uint8_t *dma = staging.get(size);

    size_t written = 0;
    while (written < size) {
        size_t available = 0;
        const uint8_t *data = span(written, available);
        if (!data || !available) break;

        size_t chunk = std::min(available, size - written);
        if (dma) {
            chunk = std::min(WriteStagingBuffer::Capacity, size - written);
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

    return written;
}

}  // namespace

size_t write_buffer(File &file, const uint8_t *data, size_t size,
                    WriteStagingBuffer &staging) {
    return write_staged([&file](const uint8_t *bytes, size_t count) {
        return file.write(bytes, count);
    }, [data, size](size_t offset, size_t &available) {
        available = size - offset;
        return data + offset;
    }, size, staging);
}

size_t write_buffers(int descriptor, const StorageRangeWriteCommand &command,
                     size_t offset, size_t size, WriteStagingBuffer &staging) {
    return write_staged([descriptor, &command](const uint8_t *bytes, size_t count) {
        const ssize_t written = ::write(descriptor, bytes, count);
        if (written < 0 || static_cast<size_t>(written) != count) {
            log_io_error("write", command.path.c_str(), written < 0 ? errno : 0,
                         written > 0 ? static_cast<size_t>(written) : 0, count);
        }
        return written > 0 ? static_cast<size_t>(written) : 0;
    }, [&command, offset](size_t position, size_t &available) {
        return command.span(offset + position, available);
    }, size, staging);
}

}  // namespace aircannect::Storage
