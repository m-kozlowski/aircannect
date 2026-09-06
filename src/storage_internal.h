#pragma once

#include <FS.h>
#include <sys/stat.h>

#include "storage_admission.h"
#include "storage_manager.h"

namespace aircannect::Storage {

bool ensure_dir(const char *path);
bool exists(const char *path);
bool file_stat(const char *path, struct stat &info);
bool remove(const char *path);
bool rmdir(const char *path);
bool rename(const char *from, const char *to);
File open(const char *path, const char *mode);
int open_descriptor(const char *path, int flags);

// Storage-task file preparation and post-close metadata
enum class ParentDirectoryStep : uint8_t { More, Done, Failed };
ParentDirectoryStep ensure_parent_directory_step(const char *path,
                                                 size_t &cursor);
bool ensure_parent_directories(const char *path);
uint64_t file_modified(const char *path);

// Bounded caller-owned write; temporary DMA staging is released on return.
size_t write_buffer(File &file, const uint8_t *data, size_t size);
size_t write_buffer(int descriptor, const uint8_t *data, size_t size);

bool poll(bool allow_capacity_update);
bool retry_mount();

}  // namespace aircannect::Storage
