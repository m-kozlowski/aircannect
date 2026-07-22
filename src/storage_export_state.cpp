#include "storage_export_state.h"

#include <FS.h>
#include <string.h>

#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {

void storage_read_local_node_info(File &file, StorageLocalNodeInfo &out) {
    out = StorageLocalNodeInfo();
    if (!file) return;
    out.exists = true;
    out.is_dir = file.isDirectory();
    out.size = out.is_dir ? 0 : static_cast<uint64_t>(file.size());
    const time_t last_write = file.getLastWrite();
    out.mtime = last_write > 0 ? static_cast<uint64_t>(last_write) : 0;
}

StorageLocalNodeInfo storage_stat_local_node(const char *path) {
    StorageLocalNodeInfo out;
    if (!path || !path[0]) return out;

    Storage::Guard guard;
    File file = Storage::open(path, "r");
    storage_read_local_node_info(file, out);
    if (file) file.close();
    return out;
}

StorageExportStateCache::~StorageExportStateCache() {
    clear();
}

void StorageExportStateCache::clear() {
    if (entries_) Memory::free(entries_);
    entries_ = nullptr;
    count_ = 0;
    capacity_ = 0;
    loaded_ = false;
    path_[0] = '\0';
}

bool StorageExportStateCache::reserve(size_t needed) {
    if (needed <= capacity_) return true;
    size_t next = capacity_ == 0 ? 8 : capacity_ * 2;
    while (next < needed) next *= 2;

    Entry *entries = static_cast<Entry *>(
        Memory::alloc_large(sizeof(Entry) * next, false));
    if (!entries) return false;
    for (size_t i = 0; i < next; ++i) entries[i] = Entry();
    for (size_t i = 0; i < count_; ++i) entries[i] = entries_[i];
    if (entries_) Memory::free(entries_);
    entries_ = entries;
    capacity_ = next;
    return true;
}

bool StorageExportStateCache::add(uint64_t size,
                                  uint64_t mtime,
                                  const char *path) {
    if (!path) return false;
    for (size_t i = 0; i < count_; ++i) {
        Entry &entry = entries_[i];
        if (strcmp(entry.path, path) == 0) {
            entry.size = size;
            entry.mtime = mtime;
            return true;
        }
    }
    if (!reserve(count_ + 1)) return false;

    Entry &entry = entries_[count_++];
    entry.size = size;
    entry.mtime = mtime;
    copy_cstr(entry.path, sizeof(entry.path), path);
    return true;
}

bool StorageExportStateCache::load(const char *state_path) {
    if (!state_path || !state_path[0]) return false;
    if (loaded_ && strcmp(path_, state_path) == 0) return true;
    clear();
    copy_cstr(path_, sizeof(path_), state_path);
    loaded_ = true;

    File file;
    {
        Storage::Guard guard;
        file = Storage::open(state_path, "r");
    }
    if (!file) return true;

    uint8_t buffer[512] = {};
    char line[AC_STORAGE_PATH_MAX + 96] = {};
    size_t line_len = 0;
    bool ok = true;
    for (;;) {
        size_t read = 0;
        {
            Storage::Guard guard;
            read = file.read(buffer, sizeof(buffer));
        }
        if (read == 0) break;

        for (size_t i = 0; i < read; ++i) {
            const char ch = static_cast<char>(buffer[i]);
            if (ch == '\n') {
                line[line_len] = '\0';
                uint64_t size = 0;
                uint64_t mtime = 0;
                const char *path = nullptr;
                if (storage_export_parse_state_line(line, size, mtime, path) &&
                    !add(size, mtime, path)) {
                    ok = false;
                    break;
                }
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            } else {
                line_len = 0;
            }
        }
        if (!ok) break;
    }
    if (ok && line_len > 0) {
        line[line_len] = '\0';
        uint64_t size = 0;
        uint64_t mtime = 0;
        const char *path = nullptr;
        if (storage_export_parse_state_line(line, size, mtime, path) &&
            !add(size, mtime, path)) {
            ok = false;
        }
    }
    {
        Storage::Guard guard;
        file.close();
    }
    if (!ok) clear();
    return ok;
}

bool StorageExportStateCache::contains(const char *state_path,
                                       const char *path,
                                       uint64_t size,
                                       uint64_t mtime) {
    if (!state_path || !path) return false;
    if (!load(state_path)) return false;
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries_[i];
        if (entry.size == size && entry.mtime == mtime &&
            strcmp(entry.path, path) == 0) {
            return true;
        }
    }
    return false;
}

void StorageExportStateCache::note_written(
    const char *state_path,
    const char *path,
    uint64_t size,
    uint64_t mtime,
    StorageExportStateWriteMode mode) {
    if (!loaded_ || !state_path || strcmp(path_, state_path) != 0) {
        return;
    }
    if (mode == StorageExportStateWriteMode::Replace) count_ = 0;
    (void)add(size, mtime, path);
}

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
