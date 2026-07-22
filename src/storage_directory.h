#pragma once

#include <FS.h>
#include <stdint.h>
#include <time.h>

#include "storage_path.h"

namespace aircannect {

struct StorageDirChild {
    char name[AC_STORAGE_PATH_MAX] = {};
    bool is_dir = false;
    uint64_t size = 0;
    time_t last_write = 0;
};

bool storage_read_next_dir_child(File &dir, StorageDirChild &out);
bool storage_skip_dir_children(File &dir, uint32_t count);

}  // namespace aircannect
