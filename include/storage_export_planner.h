#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

#include "storage_export_plan.h"

namespace aircannect {

enum class StorageExportPlannerScope : uint8_t {
    FullCard,
    SleepHq,
};

enum class StorageExportPlannerResult : uint8_t {
    Item,
    Yield,
    DecisionRequired,
    Done,
    Error,
};

enum class StorageExportPlannerItemKind : uint8_t {
    File,
    DatalogDayComplete,
};

struct StorageExportPlannerConfig {
    StorageExportPlannerScope scope = StorageExportPlannerScope::FullCard;
    const char *state_dir = nullptr;
    StorageExportStateCache *state_cache = nullptr;
    const char *latest_datalog_day = nullptr;
    const char *only_datalog_day = nullptr;
    uint32_t max_datalog_days = 0;
    bool skip_completed_finalized_datalog_days = false;
    bool trust_completed_finalized_datalog_days = false;
    bool require_pending_datalog_file = false;
    bool defer_datalog_day_decision = false;
};

struct StorageExportPlannerItem {
    StorageExportPlannerItemKind kind = StorageExportPlannerItemKind::File;
    char path[AC_STORAGE_PATH_MAX] = {};
    char datalog_day[9] = {};
    StorageLocalNodeInfo info;
    char state_path[AC_STORAGE_PATH_MAX] = {};
    StorageExportStateWriteMode state_write_mode =
        StorageExportStateWriteMode::Append;
    bool local_state_complete = false;
    bool force_export = false;
};

class StorageExportPlanner {
public:
    StorageExportPlanner() = default;
    ~StorageExportPlanner();

    bool begin(const StorageExportPlannerConfig &config,
               char *error_out,
               size_t error_out_size);
    void reset();

    StorageExportPlannerResult next(StorageExportPlannerItem &out,
                                    char *error_out,
                                    size_t error_out_size);

    bool pending_datalog_day_decision(char *day_out,
                                      size_t day_out_size,
                                      bool &local_complete_out) const;
    bool resolve_datalog_day_decision(bool force_export,
                                      char *error_out,
                                      size_t error_out_size);

private:
    enum class DatalogDaySelection : uint8_t {
        Selected,
        DecisionRequired,
        Done,
        Error,
    };

    struct WalkFrame {
        char path[AC_STORAGE_PATH_MAX] = {};
        uint32_t next_index = 0;
        bool opened = false;
        File dir;
    };

    struct DatalogDay {
        char day[9] = {};
        char path[AC_STORAGE_PATH_MAX] = {};
    };

    bool ensure_walk_stack(char *error_out, size_t error_out_size);
    void close_walk();
    void release_walk_stack();
    bool push_dir(const char *path, char *error_out, size_t error_out_size);
    bool ensure_dir_open(WalkFrame &frame,
                         char *error_out,
                         size_t error_out_size);

    bool build_file_item(const char *path,
                         bool force_export,
                         StorageExportPlannerItem &out,
                         char *error_out,
                         size_t error_out_size);
    bool state_contains(const char *state_path,
                        const char *path,
                        uint64_t size,
                        uint64_t mtime);

    bool datalog_day_finalized(const char *day) const;
    bool datalog_day_done(const char *day) const;
    bool datalog_day_has_pending_files(const DatalogDay &day,
                                       char *error_out,
                                       size_t error_out_size);
    StorageExportPlannerResult next_full_card(StorageExportPlannerItem &out,
                                              char *error_out,
                                              size_t error_out_size,
                                              uint32_t &budget);
    StorageExportPlannerResult next_sleep_hq(StorageExportPlannerItem &out,
                                             char *error_out,
                                             size_t error_out_size,
                                             uint32_t &budget);
    StorageExportPlannerResult next_walk_item(StorageExportPlannerItem &out,
                                              char *error_out,
                                              size_t error_out_size,
                                              uint32_t &budget);

    bool reserve_datalog_days(size_t needed,
                              char *error_out,
                              size_t error_out_size);
    bool add_datalog_day(const char *day,
                         char *error_out,
                         size_t error_out_size);
    StorageExportPlannerResult scan_datalog_days(char *error_out,
                                                 size_t error_out_size,
                                                 uint32_t &budget);
    DatalogDaySelection select_next_datalog_day(char *error_out,
                                                size_t error_out_size);
    void activate_datalog_day(const DatalogDay &day, bool force_export);

    StorageExportPlannerConfig config_;
    char state_dir_[AC_STORAGE_PATH_MAX] = {};
    char latest_datalog_day_[9] = {};
    char only_datalog_day_[9] = {};
    bool started_ = false;

    WalkFrame *walk_stack_ = nullptr;
    size_t walk_depth_ = 0;
    size_t walk_capacity_ = 0;
    size_t root_index_ = 0;

    File datalog_scan_dir_;
    bool datalog_scan_opened_ = false;
    bool datalog_scan_complete_ = false;
    uint32_t datalog_scan_next_index_ = 0;
    DatalogDay *datalog_days_ = nullptr;
    size_t datalog_day_count_ = 0;
    size_t datalog_day_capacity_ = 0;
    size_t datalog_day_index_ = 0;
    uint32_t datalog_days_started_ = 0;
    bool day_active_ = false;
    bool day_force_export_ = false;
    size_t day_root_index_ = 0;
    char day_path_[AC_STORAGE_PATH_MAX] = {};
    char day_name_[9] = {};

    bool day_decision_pending_ = false;
    bool pending_day_local_complete_ = false;
    bool pending_day_has_files_ = false;
};

}  // namespace aircannect
