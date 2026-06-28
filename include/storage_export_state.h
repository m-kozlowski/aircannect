#pragma once

#include <stddef.h>
#include <stdint.h>

#include "storage_export_plan.h"

namespace aircannect {

bool storage_export_ensure_dir(const char *path);
bool storage_export_ensure_state_dir(const char *state_dir);

bool storage_export_datalog_day_done_path(const char *state_dir,
                                          const char *day,
                                          char *out,
                                          size_t out_size);
bool storage_export_datalog_day_done(const char *state_dir,
                                     const char *day);
bool storage_export_mark_datalog_day_done(const char *state_dir,
                                          const char *day);

bool storage_export_write_state_line(StorageExportStateCache *cache,
                                     const char *state_dir,
                                     const char *state_path,
                                     const char *local_path,
                                     uint64_t size,
                                     uint64_t mtime,
                                     StorageExportStateWriteMode mode,
                                     const char *line,
                                     bool skip_if_cached);

}  // namespace aircannect
