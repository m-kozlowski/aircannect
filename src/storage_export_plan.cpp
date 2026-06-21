#include "storage_export_plan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calendar_utils.h"
#include "crc32.h"
#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

const StorageExportRoot EXPORT_ROOTS[] = {
    {"/DATALOG", true},
    {"/SETTINGS", true},
    {"/STR.edf", false},
    {"/Identification.json", false},
    {"/Identification.crc", false},
    {"/journal.jnl", false},
};

bool append_literal(char *out, size_t out_size, size_t &pos,
                    const char *text) {
    for (const char *p = text ? text : ""; *p; ++p) {
        if (pos + 1 >= out_size) return false;
        out[pos++] = *p;
    }
    out[pos] = '\0';
    return true;
}

bool append_safe_bucket_text(char *out, size_t out_size, size_t &pos,
                             const char *text) {
    bool wrote = false;
    for (const char *p = text ? text : ""; *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == '/') {
            if (!wrote) continue;
            if (pos > 0 && out[pos - 1] == '-') continue;
            if (pos + 1 >= out_size) return false;
            out[pos++] = '-';
            wrote = true;
        } else if (isalnum(ch)) {
            if (pos + 1 >= out_size) return false;
            out[pos++] = static_cast<char>(ch);
            wrote = true;
        } else {
            if (!wrote || (pos > 0 && out[pos - 1] == '-')) continue;
            if (pos + 1 >= out_size) return false;
            out[pos++] = '-';
            wrote = true;
        }
    }
    while (pos > 0 && out[pos - 1] == '-') pos--;
    out[pos] = '\0';
    return wrote;
}

bool build_singleton_state_bucket(const char *prefix,
                                  const char *path,
                                  char *out,
                                  size_t out_size) {
    if (!prefix || !path || !out || out_size == 0) return false;
    size_t pos = 0;
    if (!append_literal(out, out_size, pos, prefix)) return false;
    return append_safe_bucket_text(out, out_size, pos, path);
}

bool parse_state_line(char *line,
                      uint64_t &size,
                      uint64_t &mtime,
                      const char *&path) {
    if (!line) return false;
    char *first_tab = strchr(line, '\t');
    char *second_tab = first_tab ? strchr(first_tab + 1, '\t') : nullptr;
    char *last_tab = strrchr(line, '\t');
    if (!first_tab || !second_tab || !last_tab || last_tab == line) {
        return false;
    }

    *first_tab = '\0';
    *second_tab = '\0';
    char *end = nullptr;
    const unsigned long long parsed_size = strtoull(line, &end, 10);
    if (!end || *end != '\0') return false;
    end = nullptr;
    const unsigned long long parsed_mtime =
        strtoull(first_tab + 1, &end, 10);
    if (!end || *end != '\0') return false;

    size = static_cast<uint64_t>(parsed_size);
    mtime = static_cast<uint64_t>(parsed_mtime);
    path = last_tab + 1;
    return path && *path;
}

}  // namespace

size_t storage_export_root_count() {
    return sizeof(EXPORT_ROOTS) / sizeof(EXPORT_ROOTS[0]);
}

const StorageExportRoot &storage_export_root_at(size_t index) {
    return EXPORT_ROOTS[index];
}

bool storage_export_all_digits(const char *text, size_t len) {
    if (!text || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

bool storage_export_parse_yyyymmdd(const char *text,
                                   int &year,
                                   unsigned &month,
                                   unsigned &day) {
    if (!text || strlen(text) != 8 ||
        !storage_export_all_digits(text, 8)) {
        return false;
    }
    year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
           (text[2] - '0') * 10 + (text[3] - '0');
    month = (text[4] - '0') * 10 + (text[5] - '0');
    day = (text[6] - '0') * 10 + (text[7] - '0');
    return month >= 1 && month <= 12 && day >= 1 &&
           day <= calendar_days_in_month(year, static_cast<int>(month));
}

bool storage_export_is_datalog_day_name(const char *name) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    return storage_export_parse_yyyymmdd(name, year, month, day);
}

bool storage_export_datalog_day_allowed(const char *name,
                                        uint32_t cutoff_days) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!storage_export_parse_yyyymmdd(name, year, month, day)) return false;
    const time_t now = time(nullptr);
    if (now < AC_STORAGE_EXPORT_VALID_TIME_MIN_EPOCH) return true;
    const int64_t today_days = static_cast<int64_t>(now / 86400);
    const int64_t dir_days = calendar_days_from_civil(year, month, day);
    return dir_days >= today_days - static_cast<int64_t>(cutoff_days);
}

bool storage_export_path_starts_with(const char *path, const char *prefix) {
    if (!path || !prefix) return false;
    const size_t len = strlen(prefix);
    return strncmp(path, prefix, len) == 0;
}

bool storage_export_datalog_day_from_path(const char *path,
                                          char *out,
                                          size_t out_size) {
    if (!path || !out || out_size < 9) return false;
    if (!storage_export_path_starts_with(path,
                                         AC_STORAGE_EXPORT_DATALOG_PREFIX)) {
        return false;
    }
    const char *day = path + strlen(AC_STORAGE_EXPORT_DATALOG_PREFIX);
    if (strlen(day) != 8 || !storage_export_all_digits(day, 8)) {
        return false;
    }
    memcpy(out, day, 8);
    out[8] = '\0';
    return true;
}

bool storage_export_latest_datalog_day_path(char *out,
                                            size_t out_size,
                                            char *error_out,
                                            size_t error_out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    File dir;
    StorageLocalNodeInfo dir_info;
    {
        Storage::Guard guard;
        dir = Storage::open("/DATALOG", "r");
        storage_read_local_node_info(dir, dir_info);
    }
    if (!dir_info.exists) return true;
    if (!dir_info.is_dir) {
        Storage::Guard guard;
        dir.close();
        return true;
    }

    char best[9] = {};
    for (;;) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(dir, child)) break;
        if (child.is_dir && storage_export_is_datalog_day_name(child.name) &&
            (best[0] == '\0' || strcmp(child.name, best) > 0)) {
            copy_cstr(best, sizeof(best), child.name);
        }
    }
    {
        Storage::Guard guard;
        dir.close();
    }
    if (!best[0]) return true;
    const int written = snprintf(out, out_size, "/DATALOG/%s", best);
    if (written <= 0 || static_cast<size_t>(written) >= out_size) {
        copy_cstr(error_out, error_out_size, "latest_day_path_too_long");
        return false;
    }
    return true;
}

bool storage_export_build_done_path(const char *state_dir,
                                    const char *day,
                                    char *out,
                                    size_t out_size) {
    if (!state_dir || !state_dir[0] || !day || !out || out_size == 0 ||
        strlen(day) != 8 || !storage_export_all_digits(day, 8)) {
        return false;
    }
    const int written = snprintf(out, out_size, "%s/%s.done",
                                 state_dir, day);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool storage_export_build_state_path(const char *state_dir,
                                     const char *local_path,
                                     char *out,
                                     size_t out_size,
                                     StorageExportStateWriteMode *mode_out) {
    if (!state_dir || !state_dir[0] || !local_path || !out || out_size == 0) {
        return false;
    }

    StorageExportStateWriteMode mode = StorageExportStateWriteMode::Replace;
    char bucket[AC_STORAGE_NAME_MAX] = {};
    if (storage_export_path_starts_with(local_path,
                                        AC_STORAGE_EXPORT_DATALOG_PREFIX)) {
        const char *day =
            local_path + strlen(AC_STORAGE_EXPORT_DATALOG_PREFIX);
        if (storage_export_all_digits(day, 8) &&
            (day[8] == '/' || day[8] == '\0')) {
            memcpy(bucket, day, 8);
            bucket[8] = '\0';
        } else {
            copy_cstr(bucket, sizeof(bucket), "datalog");
        }
        mode = StorageExportStateWriteMode::Append;
    } else if (storage_export_path_starts_with(local_path, "/SETTINGS/") ||
               strcmp(local_path, "/SETTINGS") == 0) {
        const char *settings_path = strcmp(local_path, "/SETTINGS") == 0
            ? "root"
            : local_path + strlen("/SETTINGS/");
        if (!build_singleton_state_bucket("settings-",
                                          settings_path,
                                          bucket,
                                          sizeof(bucket))) {
            return false;
        }
    } else {
        const char *root_path = local_path[0] == '/' ? local_path + 1
                                                     : local_path;
        if (!build_singleton_state_bucket("root-",
                                          root_path,
                                          bucket,
                                          sizeof(bucket))) {
            return false;
        }
    }

    const int written = snprintf(out, out_size, "%s/%s.state",
                                 state_dir, bucket);
    if (mode_out) *mode_out = mode;
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool storage_export_build_relative_file_path(const char *local_path,
                                             char *dir_out,
                                             size_t dir_out_size,
                                             char *name_out,
                                             size_t name_out_size) {
    if (!local_path || local_path[0] != '/' || !dir_out || !name_out) {
        return false;
    }
    const char *base = storage_basename_from_path(local_path);
    if (!base || !base[0]) return false;
    copy_cstr(name_out, name_out_size, base);

    const char *slash = strrchr(local_path, '/');
    if (!slash || slash == local_path) {
        copy_cstr(dir_out, dir_out_size, "./");
        return true;
    }
    const size_t dir_len = static_cast<size_t>(slash - local_path);
    if (dir_len + 3 >= dir_out_size) return false;
    dir_out[0] = '.';
    memcpy(dir_out + 1, local_path, dir_len);
    dir_out[dir_len + 1] = '/';
    dir_out[dir_len + 2] = '\0';
    return true;
}

void storage_export_hash_update_cstr(uint32_t &crc, const char *text) {
    if (!text) text = "";
    crc = crc32_ieee_update_state(
        crc, reinterpret_cast<const uint8_t *>(text), strlen(text));
}

uint64_t storage_export_current_epoch_seconds_or_zero() {
    const time_t now = time(nullptr);
    return now >= AC_STORAGE_EXPORT_VALID_TIME_MIN_EPOCH
        ? static_cast<uint64_t>(now)
        : 0;
}

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
                if (parse_state_line(line, size, mtime, path) &&
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
        if (parse_state_line(line, size, mtime, path) &&
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
        if (entry.size == size &&
            entry.mtime == mtime &&
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

}  // namespace aircannect
