#include "report_result_cache_files.h"

#include "report_cache_paths.h"
#include "report_plot_payload.h"
#include "storage_manager.h"

namespace aircannect {

bool result_plot_cache_exists_for_etag(uint64_t night_start_ms,
                                       const char *etag) {
    Storage::Guard g;
    if (!Storage::mounted()) return false;

    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_etag(night_start_ms,
                                         etag,
                                         path,
                                         sizeof(path))) {
        return false;
    }

    return Storage::exists(path);
}

bool load_result_json_cache_for_etag(uint64_t night_start_ms,
                                     const char *etag,
                                     LargeTextBuffer &out) {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_json_cache_path_for_etag(night_start_ms,
                                         etag,
                                         path,
                                         sizeof(path))) {
        return false;
    }

    return load_result_json_cache_path(path, out);
}

bool load_result_json_cache_path(const char *path, LargeTextBuffer &out) {
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

bool load_result_plot_cache_for_etag(uint64_t night_start_ms,
                                     const char *etag,
                                     ReportSpoolBuffer &out) {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_etag(night_start_ms,
                                         etag,
                                         path,
                                         sizeof(path))) {
        return false;
    }

    return load_result_plot_cache_path(path, out);
}

bool load_result_plot_cache_path(const char *path, ReportSpoolBuffer &out) {
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

}  // namespace aircannect
