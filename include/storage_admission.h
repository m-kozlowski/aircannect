#pragma once

#include <stdint.h>

namespace aircannect {

enum class StorageAdmissionKind : uint8_t {
    PathMutation,
    Maintenance,
    BrowserRead,
    BrowserDownload,
    Upload,
};

enum class StorageAdmissionResult : uint8_t {
    Accepted,
    Busy,
    Unavailable,
};

constexpr const char *storage_admission_error(
    StorageAdmissionResult result) {
    return result == StorageAdmissionResult::Unavailable
        ? "storage_unavailable"
        : "storage_busy";
}

constexpr StorageAdmissionResult storage_request_availability(
    bool mounted,
    bool therapy_active) {
    if (!mounted) return StorageAdmissionResult::Unavailable;
    return therapy_active
        ? StorageAdmissionResult::Busy
        : StorageAdmissionResult::Accepted;
}

namespace StorageService {

StorageAdmissionResult storage_request_admission(StorageAdmissionKind kind);

}  // namespace StorageService

}  // namespace aircannect
