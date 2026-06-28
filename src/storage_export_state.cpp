#include "storage_export_state.h"

#include <FS.h>
#include <string.h>

#include "storage_manager.h"

namespace aircannect {

bool storage_export_ensure_dir(const char *path) {
    if (!path || path[0] != '/') return false;
    char current[AC_STORAGE_PATH_MAX] = {};
    size_t len = 0;
    current[len++] = '/';
    current[len] = '\0';
    const char *segment = path + 1;
    for (const char *p = segment;; ++p) {
        if (*p != '/' && *p != '\0') continue;
        const size_t seg_len = static_cast<size_t>(p - segment);
        if (seg_len > 0) {
            if (len > 1) current[len++] = '/';
            if (len + seg_len >= sizeof(current)) return false;
            memcpy(current + len, segment, seg_len);
            len += seg_len;
            current[len] = '\0';
            if (!Storage::ensure_dir(current)) return false;
        }
        if (*p == '\0') break;
        segment = p + 1;
    }
    return true;
}

bool storage_export_ensure_state_dir(const char *state_dir) {
    return state_dir && state_dir[0] && storage_export_ensure_dir(state_dir);
}

bool storage_export_datalog_day_done_path(const char *state_dir,
                                          const char *day,
                                          char *out,
                                          size_t out_size) {
    return storage_export_build_done_path(state_dir, day, out, out_size);
}

bool storage_export_datalog_day_done(const char *state_dir,
                                     const char *day) {
    char path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_export_datalog_day_done_path(state_dir,
                                              day,
                                              path,
                                              sizeof(path))) {
        return false;
    }
    const StorageLocalNodeInfo info = storage_stat_local_node(path);
    return info.exists && !info.is_dir;
}

bool storage_export_mark_datalog_day_done(const char *state_dir,
                                          const char *day) {
    char path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_export_datalog_day_done_path(state_dir,
                                              day,
                                              path,
                                              sizeof(path))) {
        return false;
    }
    if (!storage_export_ensure_state_dir(state_dir)) return false;
    Storage::Guard guard;
    File file = Storage::open(path, "w");
    if (!file) return false;
    const size_t written = file.printf("done\n");
    file.close();
    return written != 0;
}

bool storage_export_write_state_line(StorageExportStateCache *cache,
                                     const char *state_dir,
                                     const char *state_path,
                                     const char *local_path,
                                     uint64_t size,
                                     uint64_t mtime,
                                     StorageExportStateWriteMode mode,
                                     const char *line,
                                     bool skip_if_cached) {
    if (!state_path || !state_path[0] || !line) return false;
    if (skip_if_cached && cache &&
        cache->contains(state_path, local_path, size, mtime)) {
        return true;
    }
    if (!storage_export_ensure_state_dir(state_dir)) return false;

    Storage::Guard guard;
    File file = Storage::open(
        state_path,
        mode == StorageExportStateWriteMode::Replace ? "w" : "a");
    if (!file) return false;
    const size_t line_written = file.print(line);
    const size_t newline_written = file.print('\n');
    file.close();
    if (line_written == 0 || newline_written == 0) return false;

    if (cache) {
        cache->note_written(state_path, local_path, size, mtime, mode);
    }
    return true;
}

}  // namespace aircannect
