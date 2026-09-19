#pragma once

#include <stddef.h>

namespace aircannect::Storage {

// Storage-task-only. Expected missing paths and ordinary EOF are not I/O errors.
void log_io_error(const char *operation, const char *path, int error,
                  size_t actual = 0, size_t requested = 0);

}  // namespace aircannect::Storage
