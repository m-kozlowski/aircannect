#include "storage_path.h"

#include <string.h>
#include <stdio.h>

namespace aircannect {

const char *storage_basename_from_path(const char *path) {
    if (!path || !*path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
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

bool storage_resolve_user_path(const char *base,
                               const char *path,
                               char *out,
                               size_t out_size) {
    if (!base || !storage_user_path_valid(base) || !path || !out ||
        out_size < 2) {
        return false;
    }

    out[0] = '/';
    out[1] = '\0';
    size_t out_length = 1;

    auto consume = [&](const char *input) {
        const char *cursor = input;
        while (*cursor) {
            while (*cursor == '/') cursor++;
            if (!*cursor) break;

            const char *segment = cursor;
            while (*cursor && *cursor != '/') cursor++;
            const size_t length = static_cast<size_t>(cursor - segment);
            if (length == 1 && segment[0] == '.') continue;
            if (length == 2 && segment[0] == '.' && segment[1] == '.') {
                if (out_length > 1) {
                    char *slash = strrchr(out, '/');
                    out_length = slash == out
                        ? 1
                        : static_cast<size_t>(slash - out);
                    out[out_length] = '\0';
                }
                continue;
            }

            for (size_t i = 0; i < length; ++i) {
                if (static_cast<unsigned char>(segment[i]) < 0x20 ||
                    segment[i] == '\\') {
                    return false;
                }
            }

            const size_t separator = out_length > 1 ? 1 : 0;
            if (out_length + separator + length >= out_size) return false;
            if (separator) out[out_length++] = '/';
            memcpy(out + out_length, segment, length);
            out_length += length;
            out[out_length] = '\0';
        }
        return true;
    };

    if (path[0] != '/' && !consume(base)) return false;
    if (!consume(path)) return false;
    return storage_user_path_valid(out);
}

void storage_normalize_path(char *path) {
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

}  // namespace aircannect
