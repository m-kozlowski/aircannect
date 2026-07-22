#include "storage_export_plan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calendar_utils.h"
#include "crc32.h"
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

bool storage_export_datalog_day_allowed_at(const char *name,
                                           uint64_t now_epoch,
                                           uint32_t cutoff_days) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!storage_export_parse_yyyymmdd(name, year, month, day)) return false;
    if (now_epoch <
        static_cast<uint64_t>(AC_STORAGE_EXPORT_VALID_TIME_MIN_EPOCH)) {
        return true;
    }
    const int64_t today_days = static_cast<int64_t>(now_epoch / 86400);
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

bool storage_export_datalog_day_from_descendant(const char *path,
                                                char *out,
                                                size_t out_size) {
    if (!path || !out || out_size < 9 ||
        !storage_export_path_starts_with(
            path,
            AC_STORAGE_EXPORT_DATALOG_PREFIX)) {
        return false;
    }

    const char *day = path + strlen(AC_STORAGE_EXPORT_DATALOG_PREFIX);
    if (!storage_export_all_digits(day, 8) ||
        (day[8] != '\0' && day[8] != '/')) {
        return false;
    }
    memcpy(out, day, 8);
    out[8] = '\0';
    return storage_export_is_datalog_day_name(out);
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

bool storage_export_state_path_datalog_day(const char *state_dir,
                                           const char *state_path,
                                           char *day_out,
                                           size_t day_out_size) {
    if (!state_dir || !state_dir[0] || !state_path || !day_out ||
        day_out_size < 9) {
        return false;
    }

    const size_t state_dir_length = strlen(state_dir);
    if (strncmp(state_path, state_dir, state_dir_length) != 0 ||
        state_path[state_dir_length] != '/') {
        return false;
    }

    const char *name = state_path + state_dir_length + 1;
    static constexpr char STATE_SUFFIX[] = ".state";
    if (strlen(name) != 8 + sizeof(STATE_SUFFIX) - 1 ||
        !storage_export_all_digits(name, 8) ||
        strcmp(name + 8, STATE_SUFFIX) != 0) {
        return false;
    }

    memcpy(day_out, name, 8);
    day_out[8] = '\0';
    return storage_export_is_datalog_day_name(day_out);
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

bool storage_export_parse_state_line(char *line,
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

}  // namespace aircannect
