#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "storage_export_inventory.h"

namespace aircannect {

enum class StorageExportPlannerScope : uint8_t {
    FullCard,
    SleepHq,
};

enum class StorageExportPlannerResult : uint8_t {
    Item,
    Yield,
    InventoryRequired,
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
    const char *only_datalog_day = nullptr;
    uint64_t now_epoch = 0;
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
    bool begin(
        const StorageExportPlannerConfig &config,
        std::shared_ptr<const StorageExportInventoryView> inventory,
        char *error_out,
        size_t error_out_size);
    void reset();

    StorageExportPlannerResult next(StorageExportPlannerItem &out,
                                    char *error_out,
                                    size_t error_out_size);

    bool pending_datalog_day_inventory(char *day_out,
                                       size_t day_out_size) const;
    bool provide_datalog_day_inventory(
        std::shared_ptr<const StorageExportInventoryView> inventory,
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
        InventoryRequired,
        DecisionRequired,
        Done,
        Error,
    };

    bool build_file_item(const StorageExportInventoryEntryView &entry,
                         const char *datalog_day,
                         bool force_export,
                         StorageExportPlannerItem &out,
                         char *error_out,
                         size_t error_out_size) const;
    bool datalog_day_allowed(const char *day) const;
    bool datalog_day_finalized(const char *day) const;
    bool datalog_day_skipped(const char *day) const;
    bool datalog_day_inventory_loaded(const char *day) const;
    StorageExportPlannerResult require_datalog_day_inventory(
        const char *day,
        char *error_out,
        size_t error_out_size);
    void release_datalog_day_inventory();
    StorageExportPlannerResult emit_datalog_day_complete(
        const char *day,
        StorageExportPlannerItem &out,
        char *error_out,
        size_t error_out_size) const;

    StorageExportPlannerResult next_full_card(StorageExportPlannerItem &out,
                                              char *error_out,
                                              size_t error_out_size,
                                              uint32_t &budget);
    StorageExportPlannerResult next_sleep_hq(StorageExportPlannerItem &out,
                                             char *error_out,
                                             size_t error_out_size,
                                             uint32_t &budget);
    DatalogDaySelection select_next_datalog_day(char *error_out,
                                                size_t error_out_size);
    void activate_datalog_day(const char *day, bool force_export);

    StorageExportPlannerConfig config_;
    std::shared_ptr<const StorageExportInventoryView> catalog_;
    std::shared_ptr<const StorageExportInventoryView> day_inventory_;
    char state_dir_[AC_STORAGE_PATH_MAX] = {};
    char only_datalog_day_[9] = {};
    bool started_ = false;

    size_t catalog_source_index_ = 0;
    size_t day_source_index_ = 0;
    bool day_complete_emitted_ = false;
    char inventory_required_day_[9] = {};

    size_t datalog_day_index_ = 0;
    uint32_t datalog_days_started_ = 0;
    bool day_active_ = false;
    bool day_force_export_ = false;
    size_t day_metadata_index_ = 0;
    char day_name_[9] = {};

    bool day_decision_pending_ = false;
    bool pending_day_local_complete_ = false;
    bool pending_day_has_files_ = false;
};

}  // namespace aircannect
