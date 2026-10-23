#pragma once

#include <FS.h>
#include <sys/stat.h>

#include "storage_admission.h"
#include "storage_manager.h"
#include "storage_io_diagnostics.h"

namespace aircannect { struct StorageRangeWriteCommand; }

namespace aircannect::Storage {

// Explicitly confirmed format, called before begin() and any SD owner starts.
bool format_card_on_boot(char *error, size_t size);

// Storage task only, after all local filesystem users have closed their files.
void unmount();

bool ensure_dir(const char *path);
bool exists(const char *path);
bool file_stat(const char *path, struct stat &info);
bool remove(const char *path);
bool rmdir(const char *path);
bool rename(const char *from, const char *to);
File open(const char *path, const char *mode);
File open_observed_file(const char *mount_point, const char *path,
                         const char *mode);
int open_descriptor(const char *path, int flags);
int close_descriptor(const char *path, int descriptor);

// Registered before the worker starts; callbacks run on the storage task.
using PathChangeCallback = void (*)(const char *path);
void set_path_change_callback(PathChangeCallback callback);

// Before mutations; nullptr releases the reader for remount/resource pressure.
using ReadReleaseCallback = bool (*)(const char *path);
void set_read_release_callback(ReadReleaseCallback callback);
bool release_read_handle(const char *path = nullptr);

// At most three retained range-write descriptors, including an active retaining
// write. One-shot writes reserve no cache slot; resource pressure evicts on open.
// finish reports errors from earlier closes.
int take_write_handle(const char *path, bool reserve = true);
void close_write_handle(const char *path, int descriptor, bool retain);
bool release_write_handles();
void release_write_handle(const char *path);
bool finish_write_handles();

// Owned by one write operation, including all of its bounded worker turns.
class WriteStagingBuffer {
public:
    static constexpr size_t Capacity = 4096;
    WriteStagingBuffer() = default;
    ~WriteStagingBuffer();
    WriteStagingBuffer(const WriteStagingBuffer &) = delete;
    WriteStagingBuffer &operator=(const WriteStagingBuffer &) = delete;

    uint8_t *get(size_t write_size);
    void reset();

private:
    uint8_t *data_ = nullptr;
    bool attempted_ = false;
};

size_t write_buffers(int descriptor, const StorageRangeWriteCommand &command,
                     size_t offset, size_t size, WriteStagingBuffer &staging);

// Storage-task file preparation and post-close metadata
enum class ParentDirectoryStep : uint8_t { More, Done, Failed };
struct ParentDirectoryCursor {
    size_t offset = 0;
    bool creating = false;
};

ParentDirectoryStep ensure_parent_directory_step(const char *path,
                                                 ParentDirectoryCursor &cursor);
bool ensure_parent_directories(const char *path);
uint64_t file_modified(const char *path);

size_t write_buffer(File &file, const uint8_t *data, size_t size,
                    WriteStagingBuffer &staging);

bool poll(bool allow_capacity_update);
bool retry_mount();

}  // namespace aircannect::Storage
