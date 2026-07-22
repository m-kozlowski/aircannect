#include "report_store.h"

#include <Arduino.h>
#include <stdio.h>

#include "report_store_internal.h"
#include "report_legacy_storage.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStoreInternal {

ReportStoreStatus current;

void set_error(char *dst, size_t size, const char *error) {
    copy_cstr(dst, size, error);
}

void note_error(const char *error, uint32_t *counter) {
    if (counter) (*counter)++;

    set_error(current.last_error, sizeof(current.last_error), error);
}

}  // namespace ReportStoreInternal

namespace ReportStore {

using namespace ReportStoreInternal;

void begin() {
    ReportLegacyStorageGuard g;

    if (current.initialized) return;

    current.initialized = true;
    ensure_layout();
}

ReportStoreStatus status() {
    if (!current.initialized) begin();

    current.available = ReportLegacyStorage::mounted();
    return current;
}

bool ready() {
    if (!current.initialized) begin();

    return current.available;
}

bool ensure_layout() {
    ReportLegacyStorageGuard g;

    current.available = ReportLegacyStorage::mounted();
    if (!current.available) {
        note_error("storage_unavailable", &current.layout_errors);
        return false;
    }

    const char *dirs[] = {
        "/aircannect",
        "/aircannect/report",
        BASE_DIR,
        "/aircannect/report/v3/summary",
        "/aircannect/report/v3/coverage",
        "/aircannect/report/v3/series",
        "/aircannect/report/v3/events",
    };

    for (const char *dir : dirs) {
        if (!ReportLegacyStorage::ensure_dir(dir)) {
            note_error("layout_dir_failed", &current.layout_errors);
            return false;
        }
    }

    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

bool reset_cache_store(uint32_t &renamed) {
    ReportLegacyStorageGuard g;

    renamed = 0;
    if (!ready()) {
        note_error("storage_unavailable", &current.layout_errors);
        return false;
    }

    if (ReportLegacyStorage::exists(BASE_DIR)) {
        char trash_path[REPORT_PATH_MAX];
        const int written =
            snprintf(trash_path,
                     sizeof(trash_path),
                     "/aircannect/report/v3-trash-%lu",
                     static_cast<unsigned long>(millis()));
        if (written <= 0 ||
            static_cast<size_t>(written) >= sizeof(trash_path)) {
            note_error("bad_report_trash_path", &current.layout_errors);
            return false;
        }

        if (!ReportLegacyStorage::rename(BASE_DIR, trash_path)) {
            note_error("report_store_rename_failed", &current.layout_errors);
            return false;
        }

        renamed = 1;
    }

    if (!ensure_layout()) return false;

    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

}  // namespace ReportStore
}  // namespace aircannect
