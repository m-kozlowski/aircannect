#include "report_cache_paths.h"

#include <stdio.h>
#include <string.h>

#include "storage_manager.h"

namespace aircannect {
namespace {

const char *plot_cache_basename(const char *path) {
    if (!path) return "";

    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

bool cache_name_for_night(const char *name, uint64_t night_start_ms) {
    const char *base = plot_cache_basename(name);

    char prefix[32];
    const int written = snprintf(prefix,
                                 sizeof(prefix),
                                 "%llu-",
                                 static_cast<unsigned long long>(
                                     night_start_ms));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(prefix)) {
        return false;
    }

    const size_t prefix_len = strlen(prefix);
    return strncmp(base, prefix, prefix_len) == 0;
}

bool cache_file_extension_matches(const char *name,
                                  const char *first,
                                  const char *second) {
    const char *base = plot_cache_basename(name);
    const char *dot = strrchr(base, '.');
    if (!dot) return false;

    return strcmp(dot, first) == 0 || strcmp(dot, second) == 0;
}

bool clear_cache_dir_for_night(const char *dir,
                               uint64_t night_start_ms,
                               bool (*name_matches)(const char *,
                                                    uint64_t),
                               uint32_t &deleted) {
    deleted = 0;
    if (!night_start_ms || !dir || !name_matches) return false;

    Storage::Guard g;
    if (!Storage::mounted()) return false;

    File dir_file = Storage::open(dir, "r");
    if (!dir_file) return true;
    if (!dir_file.isDirectory()) {
        dir_file.close();
        return false;
    }

    bool ok = true;
    while (true) {
        File file = dir_file.openNextFile();
        if (!file) break;

        const bool is_dir = file.isDirectory();
        const bool match =
            !is_dir && name_matches(file.name(), night_start_ms);

        char path[REPORT_CACHE_PATH_MAX];
        const bool path_ok = match && cache_child_path(dir,
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
    dir_file.close();

    return ok;
}

}  // namespace

bool cache_child_path(const char *dir,
                      const char *child_name,
                      char *out,
                      size_t out_size) {
    if (!child_name || !child_name[0] || !out || !out_size) {
        return false;
    }

    if (child_name[0] == '/') {
        const int written = snprintf(out, out_size, "%s", child_name);
        return written > 0 && static_cast<size_t>(written) < out_size;
    }

    const int written = snprintf(out, out_size, "%s/%s", dir, child_name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool plot_cache_name_for_night(const char *name, uint64_t night_start_ms) {
    return cache_name_for_night(name, night_start_ms) &&
           cache_file_extension_matches(name, ".bin", ".tmp");
}

bool result_json_cache_name_for_night(const char *name,
                                      uint64_t night_start_ms) {
    return cache_name_for_night(name, night_start_ms) &&
           cache_file_extension_matches(name, ".json", ".tmp");
}

bool result_plot_cache_path_for_etag(uint64_t night_start_ms,
                                     const char *etag,
                                     char *path,
                                     size_t path_size) {
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

bool result_json_cache_path_for_etag(uint64_t night_start_ms,
                                     const char *etag,
                                     char *path,
                                     size_t path_size) {
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

bool clear_result_plot_cache_for_night(uint64_t night_start_ms,
                                       uint32_t &deleted) {
    return clear_cache_dir_for_night(REPORT_PLOT_CACHE_DIR,
                                     night_start_ms,
                                     plot_cache_name_for_night,
                                     deleted);
}

bool clear_result_json_cache_for_night(uint64_t night_start_ms,
                                       uint32_t &deleted) {
    return clear_cache_dir_for_night(REPORT_RESULT_JSON_CACHE_DIR,
                                     night_start_ms,
                                     result_json_cache_name_for_night,
                                     deleted);
}

}  // namespace aircannect
