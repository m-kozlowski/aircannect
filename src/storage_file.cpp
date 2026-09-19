#include <vfs_api.h>
#include <errno.h>
#include <string.h>

#include "storage_io_diagnostics.h"
#include "storage_path.h"

namespace aircannect::Storage {
namespace {

// Keep Arduino's File interface and buffering, but observe errors that its
// void flush/close API discards. No extra I/O is needed on successful transfers.
class ObservedFile final : public VFSFileImpl {
public:
    ObservedFile(VFSImpl *fs, const char *path, const char *mode)
        : VFSFileImpl(fs, path, mode) {}

    ~ObservedFile() override { close(); }

    size_t read(uint8_t *data, size_t size) override {
        errno = 0;
        const size_t actual = VFSFileImpl::read(data, size);
        if (actual < size && _f && ferror(_f)) {
            log_io_error("read", _path, errno, actual, size);
        }
        return actual;
    }

    size_t write(const uint8_t *data, size_t size) override {
        errno = 0;
        const size_t actual = VFSFileImpl::write(data, size);
        if (actual < size) log_io_error("write", _path, errno, actual, size);
        return actual;
    }

    bool seek(uint32_t offset, SeekMode mode) override {
        errno = 0;
        const bool ok = VFSFileImpl::seek(offset, mode);
        if (!ok) log_io_error("seek", _path, errno);
        return ok;
    }

    size_t size() const override {
        errno = 0;
        const size_t result = VFSFileImpl::size();
        if (errno) log_io_error("stat", _path, errno);
        return result;
    }

    time_t getLastWrite() override {
        errno = 0;
        const time_t result = VFSFileImpl::getLastWrite();
        if (errno) log_io_error("stat", _path, errno);
        return result;
    }

    void flush() override {
        if (!_f) return;

        if (fflush(_f) != 0) log_io_error("fflush", _path, errno);
        if (fsync(fileno(_f)) != 0) log_io_error("fsync", _path, errno);
    }

    void close() override {
        if (_f) {
            if (fclose(_f) != 0) log_io_error("close", _path, errno);
            _f = nullptr;
        }
        if (_d) {
            if (closedir(_d) != 0) log_io_error("closedir", _path, errno);
            _d = nullptr;
        }

        VFSFileImpl::close();
    }

    FileImplPtr openNextFile(const char *mode) override {
        if (!_d) return {};

        struct dirent *entry;
        do {
            errno = 0;
            entry = readdir(_d);
            if (!entry) {
                if (errno) log_io_error("readdir", _path, errno);
                return {};
            }
        } while (entry->d_type != DT_REG && entry->d_type != DT_DIR);

        char path[AC_STORAGE_PATH_MAX] = {};
        const char *separator = _path[strlen(_path) - 1] == '/' ? "" : "/";
        const int length = snprintf(path, sizeof(path), "%s%s%s",
                                    _path, separator, entry->d_name);

        if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
            log_io_error("open", _path, ENAMETOOLONG);
            return {};
        }

        auto file = open_observed(_fs, path, mode);
        if (!*file) log_io_error("open", path, errno);
        return file;
    }

    static FileImplPtr open_observed(VFSImpl *fs, const char *path,
                                     const char *mode) {
        errno = 0;
        return std::make_shared<ObservedFile>(fs, path, mode);
    }
};

}  // namespace

File open_observed_file(const char *mount_point, const char *path,
                         const char *mode) {
    static VFSImpl backend;
    backend.mountpoint(mount_point);
    return File(ObservedFile::open_observed(&backend, path, mode));
}

}  // namespace aircannect::Storage
