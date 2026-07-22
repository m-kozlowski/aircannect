#include "report_legacy_storage.h"

#include "storage_manager.h"

namespace aircannect {

bool ReportLegacyStorage::mounted() {
    return Storage::mounted();
}

bool ReportLegacyStorage::ensure_dir(const char *path) {
    return Storage::ensure_dir(path);
}

bool ReportLegacyStorage::exists(const char *path) {
    return Storage::exists(path);
}

bool ReportLegacyStorage::remove(const char *path) {
    return Storage::remove(path);
}

bool ReportLegacyStorage::rmdir(const char *path) {
    return Storage::rmdir(path);
}

bool ReportLegacyStorage::rename(const char *from, const char *to) {
    return Storage::rename(from, to);
}

ReportLegacyFile ReportLegacyStorage::open(const char *path,
                                           const char *mode) {
    return Storage::open(path, mode);
}

ReportLegacyStorageGuard::ReportLegacyStorageGuard() {
    Storage::lock();
}

ReportLegacyStorageGuard::~ReportLegacyStorageGuard() {
    Storage::unlock();
}

}  // namespace aircannect
