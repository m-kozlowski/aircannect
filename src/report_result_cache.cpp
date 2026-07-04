#include "report_manager.h"

#include <stdio.h>

#include "report_cache_paths.h"
#include "report_plot_payload.h"
#include "storage_manager.h"

namespace aircannect {

bool ReportManager::result_plot_cache_path_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    char *path,
    size_t path_size) const {
    return result_plot_cache_path_for_etag(night.summary.start_ms,
                                           etag,
                                           path,
                                           path_size);
}

bool ReportManager::result_plot_cache_path_for_etag(uint64_t night_start_ms,
                                                    const char *etag,
                                                    char *path,
                                                    size_t path_size) const {
    if (!path || !path_size || night_start_ms == 0 || !etag || !etag[0]) {
        return false;
    }

    const int written = snprintf(
        path,
        path_size,
        "%s/%llu-%s.bin",
        REPORT_PLOT_CACHE_DIR,
        static_cast<unsigned long long>(night_start_ms),
        etag);

    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool ReportManager::result_json_cache_path_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    char *path,
    size_t path_size) const {
    return result_json_cache_path_for_etag(night.summary.start_ms,
                                           etag,
                                           path,
                                           path_size);
}

bool ReportManager::result_json_cache_path_for_etag(uint64_t night_start_ms,
                                                    const char *etag,
                                                    char *path,
                                                    size_t path_size) const {
    if (!path || !path_size || night_start_ms == 0 || !etag || !etag[0]) {
        return false;
    }

    const int written = snprintf(
        path,
        path_size,
        "%s/%llu-%s.json",
        REPORT_RESULT_JSON_CACHE_DIR,
        static_cast<unsigned long long>(night_start_ms),
        etag);

    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool ReportManager::result_plot_cache_path(char *path,
                                           size_t path_size) const {
    return result_plot_cache_path_for_night(result_indexed_night_,
                                            result_etag_,
                                            path,
                                            path_size);
}

bool ReportManager::result_plot_cache_exists_for_night(
    const ReportIndexedNight &night,
    const char *etag) const {
    Storage::Guard g;
    if (!Storage::mounted()) return false;

    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }

    return Storage::exists(path);
}

bool ReportManager::clear_result_json_cache_for_night(
    const ReportSummaryRecord &night,
    uint32_t &deleted) const {
    deleted = 0;
    if (!night.start_ms) return false;

    Storage::Guard g;
    if (!Storage::mounted()) return false;

    File dir = Storage::open(REPORT_RESULT_JSON_CACHE_DIR, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        return false;
    }

    bool ok = true;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;

        const bool is_dir = file.isDirectory();
        const bool match =
            !is_dir && plot_cache_name_for_night(file.name(), night.start_ms);

        char path[REPORT_CACHE_PATH_MAX];
        const bool path_ok = match &&
                             cache_child_path(REPORT_RESULT_JSON_CACHE_DIR,
                                              file.name(),
                                              path,
                                              sizeof(path));
        file.close();

        if (!match) continue;

        if (!path_ok || !Storage::remove(path)) {
            ok = false;
            continue;
        }

        deleted++;
    }

    dir.close();
    return ok;
}

bool ReportManager::load_result_json_cache_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    LargeTextBuffer &out) const {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_json_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }

    return load_result_json_cache_path(path, out);
}

bool ReportManager::load_result_json_cache_path(const char *path,
                                                LargeTextBuffer &out) const {
    Storage::Guard g;
    out.clear();
    if (!path || !path[0] || !Storage::mounted() || !Storage::exists(path)) {
        return false;
    }

    File file = Storage::open(path, "r");
    if (!file) return false;

    const size_t size = static_cast<size_t>(file.size());
    if (size < 16 || size > REPORT_RESULT_JSON_CACHE_MAX ||
        !out.reserve(size + 1)) {
        file.close();
        out.clear();
        return false;
    }

    char buffer[512];
    while (file.available()) {
        const int n = file.read(reinterpret_cast<uint8_t *>(buffer),
                                sizeof(buffer));
        if (n < 0 || !out.append(buffer, static_cast<size_t>(n))) {
            file.close();
            out.clear();
            return false;
        }

        if (n == 0) break;
    }

    file.close();

    const char *json = out.c_str();
    if (out.length() != size || json[0] != '{') {
        out.clear();
        return false;
    }

    return true;
}

bool ReportManager::load_result_plot_cache_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    ReportSpoolBuffer &out) const {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }

    return load_result_plot_cache_path(path, out);
}

bool ReportManager::load_result_plot_cache_for_etag(uint64_t night_start_ms,
                                                    const char *etag,
                                                    ReportSpoolBuffer &out)
    const {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_etag(night_start_ms,
                                         etag,
                                         path,
                                         sizeof(path))) {
        return false;
    }

    return load_result_plot_cache_path(path, out);
}

bool ReportManager::load_result_plot_cache_path(const char *path,
                                                ReportSpoolBuffer &out) const {
    Storage::Guard g;
    out.clear();
    if (!path || !path[0] || !Storage::mounted() || !Storage::exists(path)) {
        return false;
    }

    File file = Storage::open(path, "r");
    if (!file) return false;

    const size_t size = static_cast<size_t>(file.size());
    out.set_max_size(size);
    if (size < 8 || !out.reserve_capacity(size)) {
        file.close();
        out.clear();
        return false;
    }

    uint8_t buffer[512];
    while (file.available()) {
        const int n = file.read(buffer, sizeof(buffer));
        if (n < 0 || !out.append(buffer, static_cast<size_t>(n))) {
            file.close();
            out.clear();
            return false;
        }

        if (n == 0) break;
    }

    file.close();

    const uint8_t *d = out.data();
    const uint32_t magic = d ? (static_cast<uint32_t>(d[0]) |
                                (static_cast<uint32_t>(d[1]) << 8) |
                                (static_cast<uint32_t>(d[2]) << 16) |
                                (static_cast<uint32_t>(d[3]) << 24))
                             : 0u;

    // Reject on a PLOT_BIN_VERSION mismatch too (the cache dir name guards
    // format changes by hand; this guards a version bump that forgot the dir).
    const uint16_t version = d ? (static_cast<uint16_t>(d[4]) |
                                  (static_cast<uint16_t>(d[5]) << 8))
                               : 0u;
    if (out.size() != size || magic != PLOT_BIN_MAGIC ||
        version != PLOT_BIN_VERSION) {
        out.clear();
        return false;
    }

    return true;
}

bool ReportManager::load_result_plot_cache() {
    return load_result_plot_cache_for_night(result_indexed_night_,
                                            result_etag_,
                                            result_plot_bin_);
}

}  // namespace aircannect
