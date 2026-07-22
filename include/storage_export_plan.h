#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "storage_export_day.h"
#include "storage_path.h"

namespace aircannect {

static constexpr uint32_t AC_STORAGE_EXPORT_DATALOG_CUTOFF_DAYS = 90;
static constexpr time_t AC_STORAGE_EXPORT_VALID_TIME_MIN_EPOCH = 1609459200;
static constexpr const char *AC_STORAGE_EXPORT_DATALOG_PREFIX = "/DATALOG/";

enum class StorageExportStateWriteMode : uint8_t {
    Append,
    Replace,
};

struct StorageExportRoot {
    const char *path;
    bool recursive;
};

struct StorageLocalNodeInfo {
    bool exists = false;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

size_t storage_export_root_count();
const StorageExportRoot &storage_export_root_at(size_t index);

bool storage_export_all_digits(const char *text, size_t len);
bool storage_export_parse_yyyymmdd(const char *text,
                                   int &year,
                                   unsigned &month,
                                   unsigned &day);
bool storage_export_is_datalog_day_name(const char *name);
bool storage_export_datalog_day_allowed_at(
    const char *name,
    uint64_t now_epoch,
    uint32_t cutoff_days = AC_STORAGE_EXPORT_DATALOG_CUTOFF_DAYS);
bool storage_export_datalog_day_allowed(const char *name,
                                        uint32_t cutoff_days =
                                            AC_STORAGE_EXPORT_DATALOG_CUTOFF_DAYS);
bool storage_export_path_starts_with(const char *path, const char *prefix);
bool storage_export_datalog_day_from_path(const char *path,
                                          char *out,
                                          size_t out_size);
bool storage_export_datalog_day_from_descendant(const char *path,
                                                char *out,
                                                size_t out_size);
bool storage_export_build_done_path(const char *state_dir,
                                    const char *day,
                                    char *out,
                                    size_t out_size);
bool storage_export_build_state_path(const char *state_dir,
                                     const char *local_path,
                                     char *out,
                                     size_t out_size,
                                     StorageExportStateWriteMode *mode = nullptr);
bool storage_export_build_relative_file_path(const char *local_path,
                                             char *dir_out,
                                             size_t dir_out_size,
                                             char *name_out,
                                             size_t name_out_size);
bool storage_export_parse_state_line(char *line,
                                     uint64_t &size,
                                     uint64_t &mtime,
                                     const char *&path);

void storage_export_hash_update_cstr(uint32_t &crc, const char *text);
uint64_t storage_export_current_epoch_seconds_or_zero();

}  // namespace aircannect
