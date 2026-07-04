#include "report_store_internal.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStoreInternal {

void sanitize_name(const char *name, char *out, size_t out_size) {
    if (!out || !out_size) return;

    size_t pos = 0;
    for (const char *p = name ? name : ""; *p && pos + 1 < out_size; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (isalnum(ch) || ch == '-' || ch == '_') {
            out[pos++] = static_cast<char>(ch);
        } else {
            out[pos++] = '_';
        }
    }
    out[pos] = '\0';
}

bool build_dir_path(const ReportStoreChunkKey &key, char *path, size_t path_size) {
    char safe_source[72];
    char safe_name[72];
    sanitize_name(key.source, safe_source, sizeof(safe_source));
    sanitize_name(key.name, safe_name, sizeof(safe_name));
    if (!safe_source[0] || !safe_name[0]) return false;

    int written;
    if (key.night_start_ms != 0) {
        written = snprintf(path,
                           path_size,
                           "%s/%s/%s/%s/%lld",
                           BASE_DIR,
                           ReportStore::kind_name(key.kind),
                           safe_source,
                           safe_name,
                           static_cast<long long>(key.night_start_ms));
    } else {
        written = snprintf(path,
                           path_size,
                           "%s/%s/%s/%s",
                           BASE_DIR,
                           ReportStore::kind_name(key.kind),
                           safe_source,
                           safe_name);
    }
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_source_dir_path(const ReportStoreChunkKey &key, char *path, size_t path_size) {
    char safe_source[72];
    sanitize_name(key.source, safe_source, sizeof(safe_source));
    if (!safe_source[0]) return false;

    const int written = snprintf(path,
                                 path_size,
                                 "%s/%s/%s",
                                 BASE_DIR,
                                 ReportStore::kind_name(key.kind),
                                 safe_source);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_chunk_path(const ReportStoreChunkKey &key, char *path, size_t path_size) {
    char dir[REPORT_PATH_MAX];
    if (!build_dir_path(key, dir, sizeof(dir))) return false;

    const int written = snprintf(path,
                                 path_size,
                                 "%s/%lld-%lld.bin",
                                 dir,
                                 static_cast<long long>(key.start_ms),
                                 static_cast<long long>(key.end_ms));
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_chunk_index_path(const ReportStoreChunkKey &key, char *path, size_t path_size) {
    char dir[REPORT_PATH_MAX];
    if (!build_dir_path(key, dir, sizeof(dir))) return false;

    const int written = snprintf(path, path_size, "%s/chunks.idx", dir);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool build_coverage_path(const char *source, char *path, size_t path_size) {
    char safe[72];
    sanitize_name(source, safe, sizeof(safe));
    if (!safe[0]) return false;

    const int written = snprintf(path, path_size, "%s/coverage/%s.idx", BASE_DIR, safe);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool ensure_chunk_dir(const ReportStoreChunkKey &key) {
    char source_dir[REPORT_PATH_MAX];
    if (!build_source_dir_path(key, source_dir, sizeof(source_dir))) {
        note_error("bad_chunk_path", &current.layout_errors);
        return false;
    }
    if (!Storage::ensure_dir(source_dir)) {
        note_error("chunk_source_dir_failed", &current.layout_errors);
        return false;
    }

    // The partitioned path adds a <night_start_ms> dir under the name level, so
    // create the name level first.
    if (key.night_start_ms != 0) {
        ReportStoreChunkKey name_key = key;
        name_key.night_start_ms = 0;

        char name_dir[REPORT_PATH_MAX];
        if (!build_dir_path(name_key, name_dir, sizeof(name_dir))) {
            note_error("bad_chunk_path", &current.layout_errors);
            return false;
        }
        if (!Storage::ensure_dir(name_dir)) {
            note_error("chunk_dir_failed", &current.layout_errors);
            return false;
        }
    }

    char dir[REPORT_PATH_MAX];
    if (!build_dir_path(key, dir, sizeof(dir))) {
        note_error("bad_chunk_path", &current.layout_errors);
        return false;
    }
    if (!Storage::ensure_dir(dir)) {
        note_error("chunk_dir_failed", &current.layout_errors);
        return false;
    }
    return true;
}

const char *path_basename(const char *path) {
    if (!path) return "";
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

bool build_child_path(const char *parent,
                      const char *child_name,
                      char *out,
                      size_t out_size) {
    if (!parent || !parent[0] || !child_name || !child_name[0] ||
        !out || !out_size) {
        return false;
    }

    if (child_name[0] == '/') {
        const int written = snprintf(out, out_size, "%s", child_name);
        return written > 0 && static_cast<size_t>(written) < out_size;
    }

    const int written = snprintf(out, out_size, "%s/%s", parent, child_name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool name_ends_with(const char *name, const char *suffix) {
    if (!name || !suffix) return false;

    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);
    return name_len >= suffix_len &&
           strcmp(name + name_len - suffix_len, suffix) == 0;
}

bool parse_dir_int64(const char *name, int64_t &out) {
    const char *base = path_basename(name);
    if (!base || !base[0]) return false;

    int64_t value = 0;
    for (const char *p = base; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const int digit = *p - '0';
        if (value > (INT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }

    out = value;
    return true;
}

bool is_report_trash_dir_name(const char *name) {
    const char *base = path_basename(name);
    return strncmp(base, REPORT_TRASH_PREFIX, strlen(REPORT_TRASH_PREFIX)) == 0;
}

}  // namespace ReportStoreInternal
}  // namespace aircannect
