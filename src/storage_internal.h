#pragma once

#include <FS.h>

#include "storage_manager.h"

namespace aircannect::Storage {

bool ensure_dir(const char *path);
bool exists(const char *path);
bool remove(const char *path);
bool rmdir(const char *path);
bool rename(const char *from, const char *to);
File open(const char *path, const char *mode);
bool poll(bool allow_capacity_update);
bool retry_mount();

}  // namespace aircannect::Storage
