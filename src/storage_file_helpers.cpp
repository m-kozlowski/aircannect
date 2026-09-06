#include "storage_internal.h"

#include <string.h>

#include "storage_path.h"

namespace aircannect::Storage {

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
        File directory = open(parent, "r");
        if (directory) {
            const bool valid = directory.isDirectory();
            directory.close();
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
    File file = open(path, "r");
    uint64_t modified = 0;
    if (file && !file.isDirectory()) {
        const time_t last_write = file.getLastWrite();
        if (last_write > 0) modified = static_cast<uint64_t>(last_write);
    }

    if (file) file.close();
    return modified;
}

}  // namespace aircannect::Storage
