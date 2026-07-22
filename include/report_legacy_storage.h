#pragma once

#include <FS.h>

namespace aircannect {

using ReportLegacyFile = File;

class ReportLegacyStorage final {
public:
    static bool mounted();
    static bool ensure_dir(const char *path);
    static bool exists(const char *path);
    static bool remove(const char *path);
    static bool rmdir(const char *path);
    static bool rename(const char *from, const char *to);
    static ReportLegacyFile open(const char *path, const char *mode);

private:
    ReportLegacyStorage() = delete;
};

class ReportLegacyStorageGuard final {
public:
    ReportLegacyStorageGuard();
    ~ReportLegacyStorageGuard();

    ReportLegacyStorageGuard(const ReportLegacyStorageGuard &) = delete;
    ReportLegacyStorageGuard &operator=(const ReportLegacyStorageGuard &) =
        delete;
};

}  // namespace aircannect
