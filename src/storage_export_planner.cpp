#include "storage_export_planner.h"

#include <stdio.h>
#include <string.h>
#include <utility>

#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t EXPORT_STEP_BUDGET = 32;

const char *const SLEEPHQ_ROOT_METADATA_FILES[] = {
    "/Identification.json",
    "/Identification.crc",
    "/STR.edf",
};

void set_error(char *out, size_t out_size, const char *error) {
    copy_cstr(out, out_size, error ? error : "planner_error");
}

}  // namespace

bool StorageExportPlanner::begin(
    const StorageExportPlannerConfig &config,
    std::shared_ptr<const StorageExportInventoryView> inventory,
    char *error_out,
    size_t error_out_size) {
    reset();
    if (!inventory ||
        inventory->kind() != StorageExportInventoryKind::Catalog) {
        set_error(error_out, error_out_size, "planner_catalog_missing");
        return false;
    }
    if (!config.state_dir || !config.state_dir[0] ||
        strcmp(config.state_dir, inventory->state_dir()) != 0) {
        set_error(error_out, error_out_size, "planner_state_dir_mismatch");
        return false;
    }
    if (config.only_datalog_day &&
        !storage_export_is_datalog_day_name(config.only_datalog_day)) {
        set_error(error_out, error_out_size, "bad_datalog_day");
        return false;
    }

    config_ = config;
    catalog_ = std::move(inventory);
    copy_cstr(state_dir_, sizeof(state_dir_), config.state_dir);
    config_.state_dir = state_dir_;
    if (config.only_datalog_day) {
        copy_cstr(only_datalog_day_,
                  sizeof(only_datalog_day_),
                  config.only_datalog_day);
        config_.only_datalog_day = only_datalog_day_;
    }
    started_ = true;
    return true;
}

void StorageExportPlanner::reset() {
    config_ = StorageExportPlannerConfig();
    catalog_.reset();
    day_inventory_.reset();
    state_dir_[0] = '\0';
    only_datalog_day_[0] = '\0';
    started_ = false;

    catalog_source_index_ = 0;
    day_source_index_ = 0;
    day_complete_emitted_ = false;
    inventory_required_day_[0] = '\0';

    datalog_day_index_ = 0;
    datalog_days_started_ = 0;
    day_active_ = false;
    day_force_export_ = false;
    day_metadata_index_ = 0;
    day_name_[0] = '\0';

    day_decision_pending_ = false;
    pending_day_local_complete_ = false;
    pending_day_has_files_ = false;
}

bool StorageExportPlanner::build_file_item(
    const StorageExportInventoryEntryView &entry,
    const char *datalog_day,
    bool force_export,
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size) const {
    if (!entry.path || !entry.info.exists || entry.info.is_dir) return false;

    out = StorageExportPlannerItem();
    out.kind = StorageExportPlannerItemKind::File;
    out.info = entry.info;
    out.local_state_complete = entry.local_state_complete;
    out.force_export = force_export;
    copy_cstr(out.path, sizeof(out.path), entry.path);
    if (datalog_day && datalog_day[0]) {
        copy_cstr(out.datalog_day, sizeof(out.datalog_day), datalog_day);
    }

    if (!storage_export_build_state_path(state_dir_,
                                         entry.path,
                                         out.state_path,
                                         sizeof(out.state_path),
                                         &out.state_write_mode)) {
        set_error(error_out, error_out_size, "state_path_failed");
        return false;
    }
    return true;
}

bool StorageExportPlanner::datalog_day_allowed(const char *day) const {
    if (!day || !day[0]) return false;
    if (only_datalog_day_[0] && strcmp(day, only_datalog_day_) != 0) {
        return false;
    }
    return storage_export_datalog_day_allowed_at(day, config_.now_epoch);
}

bool StorageExportPlanner::datalog_day_finalized(const char *day) const {
    const char *latest = catalog_ ? catalog_->latest_datalog_day() : nullptr;
    return day && latest && latest[0] && strcmp(day, latest) < 0;
}

bool StorageExportPlanner::datalog_day_skipped(const char *day) const {
    return config_.skip_completed_finalized_datalog_days &&
           datalog_day_finalized(day) && catalog_->datalog_day_done(day);
}

bool StorageExportPlanner::datalog_day_inventory_loaded(
    const char *day) const {
    return day && day_inventory_ &&
           day_inventory_->kind() == StorageExportInventoryKind::DatalogDay &&
           strcmp(day_inventory_->loaded_datalog_day(), day) == 0;
}

StorageExportPlannerResult
StorageExportPlanner::require_datalog_day_inventory(
    const char *day,
    char *error_out,
    size_t error_out_size) {
    if (!storage_export_is_datalog_day_name(day)) {
        set_error(error_out, error_out_size, "bad_datalog_day");
        return StorageExportPlannerResult::Error;
    }
    if (inventory_required_day_[0] &&
        strcmp(inventory_required_day_, day) != 0) {
        set_error(error_out, error_out_size, "planner_day_load_conflict");
        return StorageExportPlannerResult::Error;
    }

    copy_cstr(inventory_required_day_,
              sizeof(inventory_required_day_),
              day);
    return StorageExportPlannerResult::InventoryRequired;
}

void StorageExportPlanner::release_datalog_day_inventory() {
    day_inventory_.reset();
    inventory_required_day_[0] = '\0';
    day_source_index_ = 0;
}

bool StorageExportPlanner::pending_datalog_day_inventory(
    char *day_out,
    size_t day_out_size) const {
    if (!inventory_required_day_[0] || !day_out || day_out_size == 0) {
        return false;
    }

    copy_cstr(day_out, day_out_size, inventory_required_day_);
    return true;
}

bool StorageExportPlanner::provide_datalog_day_inventory(
    std::shared_ptr<const StorageExportInventoryView> inventory,
    char *error_out,
    size_t error_out_size) {
    if (!started_ || !inventory_required_day_[0]) {
        set_error(error_out, error_out_size, "planner_day_not_requested");
        return false;
    }
    if (!inventory ||
        inventory->kind() != StorageExportInventoryKind::DatalogDay ||
        strcmp(inventory->state_dir(), state_dir_) != 0 ||
        strcmp(inventory->loaded_datalog_day(),
               inventory_required_day_) != 0) {
        set_error(error_out, error_out_size, "planner_day_mismatch");
        return false;
    }

    day_inventory_ = std::move(inventory);
    inventory_required_day_[0] = '\0';
    day_source_index_ = 0;
    return true;
}

StorageExportPlannerResult StorageExportPlanner::emit_datalog_day_complete(
    const char *day,
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size) const {
    out = StorageExportPlannerItem();
    out.kind = StorageExportPlannerItemKind::DatalogDayComplete;
    copy_cstr(out.datalog_day, sizeof(out.datalog_day), day);
    const int written = snprintf(out.path,
                                 sizeof(out.path),
                                 "/DATALOG/%s",
                                 day ? day : "");
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(out.path)) {
        set_error(error_out, error_out_size, "datalog_day_path");
        return StorageExportPlannerResult::Error;
    }
    return StorageExportPlannerResult::Item;
}

StorageExportPlannerResult StorageExportPlanner::next(
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size) {
    if (!started_ || !catalog_) {
        set_error(error_out, error_out_size, "planner_not_started");
        return StorageExportPlannerResult::Error;
    }
    if (error_out && error_out_size != 0) error_out[0] = '\0';

    uint32_t budget = EXPORT_STEP_BUDGET;
    if (config_.scope == StorageExportPlannerScope::SleepHq) {
        return next_sleep_hq(out, error_out, error_out_size, budget);
    }
    return next_full_card(out, error_out, error_out_size, budget);
}

StorageExportPlannerResult StorageExportPlanner::next_full_card(
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size,
    uint32_t &budget) {
    if (day_complete_emitted_) {
        day_complete_emitted_ = false;
        release_datalog_day_inventory();
    }

    while (budget > 0) {
        if (day_active_) {
            if (!datalog_day_inventory_loaded(day_name_)) {
                return require_datalog_day_inventory(
                    day_name_, error_out, error_out_size);
            }

            while (day_source_index_ < day_inventory_->source_size() &&
                   budget > 0) {
                const size_t source_index = day_source_index_++;
                budget--;

                StorageExportInventoryEntryView entry;
                if (!day_inventory_->entry(source_index, entry)) continue;
                if (build_file_item(entry,
                                    day_name_,
                                    false,
                                    out,
                                    error_out,
                                    error_out_size)) {
                    return StorageExportPlannerResult::Item;
                }
                if (error_out && error_out[0]) {
                    return StorageExportPlannerResult::Error;
                }
            }
            if (day_source_index_ < day_inventory_->source_size()) {
                return StorageExportPlannerResult::Yield;
            }

            char completed_day[9] = {};
            copy_cstr(completed_day, sizeof(completed_day), day_name_);
            const bool had_files = day_inventory_->source_size() != 0;
            day_active_ = false;
            day_name_[0] = '\0';
            if (!had_files) {
                release_datalog_day_inventory();
                continue;
            }

            day_complete_emitted_ = true;
            return emit_datalog_day_complete(completed_day,
                                             out,
                                             error_out,
                                             error_out_size);
        }

        while (datalog_day_index_ < catalog_->datalog_day_count()) {
            const char *day = catalog_->datalog_day_at(datalog_day_index_++);
            budget--;
            if (!day || !datalog_day_allowed(day) ||
                datalog_day_skipped(day)) {
                if (budget == 0) return StorageExportPlannerResult::Yield;
                continue;
            }

            copy_cstr(day_name_, sizeof(day_name_), day);
            day_active_ = true;
            day_source_index_ = 0;
            break;
        }
        if (day_active_) continue;
        if (datalog_day_index_ < catalog_->datalog_day_count()) {
            return StorageExportPlannerResult::Yield;
        }

        while (catalog_source_index_ < catalog_->source_size() && budget > 0) {
            const size_t source_index = catalog_source_index_++;
            budget--;

            StorageExportInventoryEntryView entry;
            if (!catalog_->entry(source_index, entry)) continue;
            if (build_file_item(entry,
                                nullptr,
                                false,
                                out,
                                error_out,
                                error_out_size)) {
                return StorageExportPlannerResult::Item;
            }
            if (error_out && error_out[0]) {
                return StorageExportPlannerResult::Error;
            }
        }
        return catalog_source_index_ >= catalog_->source_size()
            ? StorageExportPlannerResult::Done
            : StorageExportPlannerResult::Yield;
    }
    return StorageExportPlannerResult::Yield;
}

bool StorageExportPlanner::pending_datalog_day_decision(
    char *day_out,
    size_t day_out_size,
    bool &local_complete_out) const {
    if (!day_decision_pending_ || !catalog_ || !day_out ||
        day_out_size == 0 ||
        datalog_day_index_ >= catalog_->datalog_day_count()) {
        return false;
    }

    copy_cstr(day_out,
              day_out_size,
              catalog_->datalog_day_at(datalog_day_index_));
    local_complete_out = pending_day_local_complete_;
    return true;
}

bool StorageExportPlanner::resolve_datalog_day_decision(
    bool force_export,
    char *error_out,
    size_t error_out_size) {
    if (!day_decision_pending_ || !catalog_ ||
        datalog_day_index_ >= catalog_->datalog_day_count()) {
        set_error(error_out, error_out_size, "datalog_decision_not_pending");
        return false;
    }

    const char *day = catalog_->datalog_day_at(datalog_day_index_);
    day_decision_pending_ = false;
    pending_day_local_complete_ = false;

    if (config_.require_pending_datalog_file && !force_export &&
        !pending_day_has_files_) {
        pending_day_has_files_ = false;
        datalog_day_index_++;
        release_datalog_day_inventory();
        return true;
    }

    pending_day_has_files_ = false;
    activate_datalog_day(day, force_export);
    return true;
}

void StorageExportPlanner::activate_datalog_day(const char *day,
                                                bool force_export) {
    copy_cstr(day_name_, sizeof(day_name_), day);
    day_active_ = true;
    day_force_export_ = force_export;
    day_metadata_index_ = 0;
    day_source_index_ = 0;
    datalog_day_index_++;
    datalog_days_started_++;
}

StorageExportPlanner::DatalogDaySelection
StorageExportPlanner::select_next_datalog_day(char *error_out,
                                              size_t error_out_size) {
    if (day_decision_pending_) {
        return DatalogDaySelection::DecisionRequired;
    }

    while (datalog_day_index_ < catalog_->datalog_day_count()) {
        if (config_.max_datalog_days != 0 &&
            datalog_days_started_ >= config_.max_datalog_days) {
            return DatalogDaySelection::Done;
        }

        const char *day = catalog_->datalog_day_at(datalog_day_index_);
        if (!day) {
            set_error(error_out, error_out_size, "datalog_day_missing");
            return DatalogDaySelection::Error;
        }
        if (!datalog_day_allowed(day)) {
            datalog_day_index_++;
            continue;
        }

        const bool trusted_done =
            config_.trust_completed_finalized_datalog_days &&
            datalog_day_finalized(day) && catalog_->datalog_day_done(day);
        if (!trusted_done && !datalog_day_inventory_loaded(day)) {
            const StorageExportPlannerResult required =
                require_datalog_day_inventory(day,
                                               error_out,
                                               error_out_size);
            return required == StorageExportPlannerResult::InventoryRequired
                ? DatalogDaySelection::InventoryRequired
                : DatalogDaySelection::Error;
        }

        const bool has_pending =
            !trusted_done && day_inventory_->datalog_day_has_pending(day);
        const bool local_complete = trusted_done || !has_pending;
        if (config_.defer_datalog_day_decision) {
            day_decision_pending_ = true;
            pending_day_local_complete_ = local_complete;
            pending_day_has_files_ = has_pending;
            return DatalogDaySelection::DecisionRequired;
        }
        if (config_.require_pending_datalog_file && !has_pending) {
            datalog_day_index_++;
            release_datalog_day_inventory();
            continue;
        }

        activate_datalog_day(day, false);
        return DatalogDaySelection::Selected;
    }
    return DatalogDaySelection::Done;
}

StorageExportPlannerResult StorageExportPlanner::next_sleep_hq(
    StorageExportPlannerItem &out,
    char *error_out,
    size_t error_out_size,
    uint32_t &budget) {
    if (day_complete_emitted_) {
        day_complete_emitted_ = false;
        release_datalog_day_inventory();
    }

    while (budget > 0) {
        if (!day_active_) {
            const DatalogDaySelection selection =
                select_next_datalog_day(error_out, error_out_size);
            switch (selection) {
                case DatalogDaySelection::Selected:
                    break;
                case DatalogDaySelection::InventoryRequired:
                    return StorageExportPlannerResult::InventoryRequired;
                case DatalogDaySelection::DecisionRequired:
                    return StorageExportPlannerResult::DecisionRequired;
                case DatalogDaySelection::Done:
                    return StorageExportPlannerResult::Done;
                case DatalogDaySelection::Error:
                    return StorageExportPlannerResult::Error;
            }
        }

        if (!datalog_day_inventory_loaded(day_name_)) {
            return require_datalog_day_inventory(
                day_name_, error_out, error_out_size);
        }

        const size_t metadata_count =
            sizeof(SLEEPHQ_ROOT_METADATA_FILES) /
            sizeof(SLEEPHQ_ROOT_METADATA_FILES[0]);
        while (day_metadata_index_ < metadata_count && budget > 0) {
            const char *path =
                SLEEPHQ_ROOT_METADATA_FILES[day_metadata_index_++];
            budget--;

            StorageExportInventoryEntryView entry;
            if (!catalog_->find_file(path, entry)) continue;
            if (build_file_item(entry,
                                day_name_,
                                true,
                                out,
                                error_out,
                                error_out_size)) {
                return StorageExportPlannerResult::Item;
            }
            if (error_out && error_out[0]) {
                return StorageExportPlannerResult::Error;
            }
        }

        while (day_source_index_ < day_inventory_->source_size() &&
               budget > 0) {
            const size_t source_index = day_source_index_++;
            budget--;

            StorageExportInventoryEntryView entry;
            if (!day_inventory_->entry(source_index, entry)) continue;
            if (build_file_item(entry,
                                day_name_,
                                day_force_export_,
                                out,
                                error_out,
                                error_out_size)) {
                return StorageExportPlannerResult::Item;
            }
            if (error_out && error_out[0]) {
                return StorageExportPlannerResult::Error;
            }
        }
        if (day_source_index_ < day_inventory_->source_size()) {
            return StorageExportPlannerResult::Yield;
        }

        char completed_day[9] = {};
        copy_cstr(completed_day, sizeof(completed_day), day_name_);
        day_active_ = false;
        day_force_export_ = false;
        day_name_[0] = '\0';
        day_complete_emitted_ = true;
        return emit_datalog_day_complete(completed_day,
                                         out,
                                         error_out,
                                         error_out_size);
    }
    return StorageExportPlannerResult::Yield;
}

}  // namespace aircannect
