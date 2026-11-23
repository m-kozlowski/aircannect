#include "storage_read_file.h"

#include <cstring>
#include <cerrno>
#include <algorithm>
#include <fcntl.h>

#include "storage_io_diagnostics.h"
#include "string_util.h"

namespace aircannect {

StorageReadFile::~StorageReadFile() {
    close();
}

bool StorageReadFile::open(const char *path, uint32_t generation,
                           size_t whole_file_limit, size_t requested_bytes) {
    const bool dma_buffered = requested_bytes >= staging_.Capacity;
    if (file_ && generation_ == generation && strcmp(path_, path) == 0 &&
        dma_requested_ == dma_buffered &&
        (has_modified_ || (whole_file_limit && size <= whole_file_limit))) {
        return true;
    }

    close();
    Storage::release_write_handle(path);
    const int descriptor = Storage::open_descriptor(path, O_RDONLY);
    if (descriptor < 0) return false;

    // FAT fstat reads the open handle; unlike path stat, it has no mtime.
    struct stat opened_stat {};
    int error = 0;
    if (::fstat(descriptor, &opened_stat) != 0) {
        error = errno ? errno : EIO;
        Storage::log_io_error("stat", path, error);
    } else if (!S_ISREG(opened_stat.st_mode) || opened_stat.st_size < 0) {
        error = EINVAL;
    }

    const bool needs_modified = !whole_file_limit ||
        static_cast<uint64_t>(opened_stat.st_size) > whole_file_limit;
    if (!error && needs_modified && !Storage::file_stat(path, opened_stat)) {
        error = errno ? errno : EIO;
    }

    if (!error) {
        file_ = fdopen(descriptor, "r");
        if (!file_) {
            error = errno ? errno : ENOMEM;
            Storage::log_io_error("open", path, error);
        }
    }
    if (error) {
        Storage::close_descriptor(path, descriptor);
        errno = error;
        return false;
    }

    dma_requested_ = dma_buffered;
    staging_.configure_reader(file_, std::min(requested_bytes, size_t(opened_stat.st_size)),
                              opened_stat.st_blksize == 0 ? 4096 : 0);

    copy_cstr(path_, sizeof(path_), path);
    generation_ = generation;
    has_modified_ = needs_modified;
    size = static_cast<size_t>(opened_stat.st_size);
    modified = needs_modified && opened_stat.st_mtime > 0
        ? static_cast<uint64_t>(opened_stat.st_mtime) : 0;
    return true;
}

size_t StorageReadFile::read(uint8_t *data, size_t length) {
    if (!file_) return 0;

    errno = 0;
    const size_t actual = fread(data, 1, length, file_);
    if (actual < length && ferror(file_)) {
        Storage::log_io_error("read", path_, errno, actual, length);
    }
    return actual;
}

bool StorageReadFile::seek(uint32_t offset) {
    if (!file_) return false;

    errno = 0;
    if (!staging_.seek_reader(file_, offset, size)) {
        Storage::log_io_error("seek", path_, errno);
        return false;
    }
    return true;
}

bool StorageReadFile::close(const char *path) {
    if (path && strcmp(path, "/") != 0) {
        const size_t length = strlen(path);
        if (strncmp(path_, path, length) != 0 ||
            (path_[length] != '\0' && path_[length] != '/')) return false;
    }

    const bool opened = file_ != nullptr;
    if (file_ && fclose(file_) != 0) {
        Storage::log_io_error("close", path_, errno);
    }
    file_ = nullptr;
    staging_.reset();
    dma_requested_ = false;
    path_[0] = '\0';
    position = size = modified = 0;
    generation_ = 0;
    has_modified_ = false;
    return opened;
}

}  // namespace aircannect
