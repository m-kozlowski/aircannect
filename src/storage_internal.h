#pragma once

#include <FS.h>
#include <sys/stat.h>

#include "storage_admission.h"
#include "storage_manager.h"

namespace aircannect { struct StorageRangeWriteCommand; }

namespace aircannect::Storage {

bool ensure_dir(const char *path);
bool exists(const char *path);
bool file_stat(const char *path, struct stat &info);
bool remove(const char *path);
bool rmdir(const char *path);
bool rename(const char *from, const char *to);
File open(const char *path, const char *mode);
int open_descriptor(const char *path, int flags);

// Storage-task-only reuse of range-write descriptors, at most three
// including the current write. Other mutations discard idle descriptors.
// take reserves room before an open; finish reports errors from earlier closes.
int take_write_handle(const char *path);
void close_write_handle(const char *path, int descriptor, bool retain);
bool release_write_handles();
void release_write_handle(const char *path);
bool finish_write_handles();
size_t write_buffers(int descriptor, const StorageRangeWriteCommand &command,
                     size_t offset, size_t size);

// Storage-task file preparation and post-close metadata
enum class ParentDirectoryStep : uint8_t { More, Done, Failed };
ParentDirectoryStep ensure_parent_directory_step(const char *path,
                                                 size_t &cursor);
bool ensure_parent_directories(const char *path);
uint64_t file_modified(const char *path);

// Bounded caller-owned write; temporary DMA staging is released on return.
size_t write_buffer(File &file, const uint8_t *data, size_t size);

bool poll(bool allow_capacity_update);
bool retry_mount();

}  // namespace aircannect::Storage
