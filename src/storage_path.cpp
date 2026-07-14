#include "storage_path.h"

#include <string.h>
#include <stdio.h>

namespace aircannect {

const char *storage_basename_from_path(const char *path) {
    if (!path || !*path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool storage_path_equals_or_under(const char *path, const char *root) {
    if (!path || !root) return false;
    const size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0) return false;
    return path[root_len] == '\0' || path[root_len] == '/';
}

bool storage_append_child_path(const char *parent,
                               const char *name,
                               char *out,
                               size_t out_size) {
    if (!parent || !name || !*name || !out || out_size == 0) return false;
    const int written = strcmp(parent, "/") == 0
        ? snprintf(out, out_size, "/%s", name)
        : snprintf(out, out_size, "%s/%s", parent, name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool storage_valid_child_name(const char *name) {
    if (!name || name[0] == 0) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char *p = name; *p; ++p) {
        const char c = *p;
        if (static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\') {
            return false;
        }
    }
    return true;
}

bool storage_user_path_valid(const char *path) {
    if (!path || path[0] != '/') return false;
    const size_t len = strlen(path);
    if (len == 0 || len >= AC_STORAGE_PATH_MAX) return false;

    size_t segment_start = 1;
    for (size_t i = 0; i <= len; ++i) {
        const char c = path[i];
        if (c == '/' || c == '\0') {
            const size_t segment_len = i - segment_start;
            if (segment_len == 0 && i != 0 && c != '\0') return false;
            if ((segment_len == 1 && path[segment_start] == '.') ||
                (segment_len == 2 && path[segment_start] == '.' &&
                 path[segment_start + 1] == '.')) {
                return false;
            }
            segment_start = i + 1;
        } else if (static_cast<unsigned char>(c) < 0x20 || c == '\\') {
            return false;
        }
    }
    return true;
}

void storage_normalize_path(char *path) {
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

}  // namespace aircannect
