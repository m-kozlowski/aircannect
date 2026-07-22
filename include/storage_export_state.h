#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

#include "storage_export_plan.h"

namespace aircannect {

void storage_read_local_node_info(File &file, StorageLocalNodeInfo &out);
StorageLocalNodeInfo storage_stat_local_node(const char *path);

class StorageExportStateCache {
public:
    ~StorageExportStateCache();

    void clear();
    bool contains(const char *state_path,
                  const char *path,
                  uint64_t size,
                  uint64_t mtime);
    void note_written(const char *state_path,
                      const char *path,
                      uint64_t size,
                      uint64_t mtime,
                      StorageExportStateWriteMode mode);

private:
    struct Entry {
        uint64_t size = 0;
        uint64_t mtime = 0;
        char path[AC_STORAGE_PATH_MAX] = {};
    };

    bool load(const char *state_path);
    bool reserve(size_t needed);
    bool add(uint64_t size, uint64_t mtime, const char *path);

    char path_[AC_STORAGE_PATH_MAX] = {};
    Entry *entries_ = nullptr;
    size_t count_ = 0;
    size_t capacity_ = 0;
    bool loaded_ = false;
};

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
