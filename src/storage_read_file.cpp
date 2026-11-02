#include "storage_read_file.h"

#include <cstring>
#include <cerrno>

#include "string_util.h"

namespace aircannect {

bool StorageReadFile::open(const char *path, uint32_t generation) {
    if (file && generation_ == generation && strcmp(path_, path) == 0) return true;

    close();
    struct stat opened_stat {};
    file = Storage::open(path, "r", &opened_stat);

    if (!file || file.isDirectory()) {
        const int error = errno;
        close();
        errno = error;
        return false;
    }

    copy_cstr(path_, sizeof(path_), path);
    generation_ = generation;

    size = static_cast<size_t>(opened_stat.st_size);
    modified = opened_stat.st_mtime > 0
        ? static_cast<uint64_t>(opened_stat.st_mtime) : 0;
    return true;
}

bool StorageReadFile::close(const char *path) {
    if (path && strcmp(path, "/") != 0) {
        const size_t length = strlen(path);
        if (strncmp(path_, path, length) != 0 ||
            (path_[length] != '\0' && path_[length] != '/')) return false;
    }

    const bool opened = static_cast<bool>(file);
    if (file) file.close();
    file = {};
    path_[0] = '\0';
    position = size = modified = 0;
    generation_ = 0;
    return opened;
}

}  // namespace aircannect
