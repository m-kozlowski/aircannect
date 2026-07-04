#include "report_cache_paths.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {
namespace {

const char *plot_cache_basename(const char *path) {
    if (!path) return "";

    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
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
    if (strncmp(base, prefix, prefix_len) != 0) return false;

    const char *dot = strrchr(base, '.');
    return dot && (strcmp(dot, ".bin") == 0 || strcmp(dot, ".tmp") == 0);
}

}  // namespace aircannect
