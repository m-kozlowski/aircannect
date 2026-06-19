#include "storage_directory.h"

#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {

bool storage_read_next_dir_child(File &dir, StorageDirChild &out) {
    out = StorageDirChild();
    Storage::Guard guard;
    File child = dir.openNextFile();
    if (!child) return false;

    copy_cstr(out.name,
              sizeof(out.name),
              storage_basename_from_path(child.name()));
    out.is_dir = child.isDirectory();
    out.size = out.is_dir ? 0 : static_cast<uint64_t>(child.size());
    out.last_write = child.getLastWrite();
    child.close();
    return true;
}

bool storage_skip_dir_children(File &dir, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(dir, child)) return false;
    }
    return true;
}

}  // namespace aircannect
