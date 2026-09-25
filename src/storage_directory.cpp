#include "storage_directory.h"

#include "storage_internal.h"
#include "string_util.h"

namespace aircannect {

bool storage_read_next_dir_child(File &dir, StorageDirChild &out) {
    out = StorageDirChild();

    String path = dir.getNextFileName(nullptr);
    if (!path.length()) return false;

    struct stat info {};
    if (!Storage::file_stat(path.c_str(), info)) return false;

    copy_cstr(out.name,
              sizeof(out.name),
              storage_basename_from_path(path.c_str()));
    out.is_dir = S_ISDIR(info.st_mode);
    out.size = out.is_dir ? 0 : static_cast<uint64_t>(info.st_size);
    out.last_write = info.st_mtime;
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
