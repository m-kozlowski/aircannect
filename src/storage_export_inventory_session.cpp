#include "storage_export_inventory_session.h"

#include <string.h>

#include "storage_export_plan.h"
#include "string_util.h"

namespace aircannect {

void StorageExportInventorySession::begin(StorageScanPort &scan_port,
                                          StorageReadPort &read_port) {
    reset();
    loader_.begin(scan_port, read_port);
}

void StorageExportInventorySession::reset() {
    loader_.reset();
    catalog_.reset();
    datalog_day_.reset();
    catalog_requested_ = false;
    day_requested_ = false;
    requested_day_[0] = '\0';
}

void StorageExportInventorySession::advance_generation() {
    next_generation_++;
    if (next_generation_ == 0) next_generation_ = 1;
}

StorageExportInventoryLoadResult
StorageExportInventorySession::poll_catalog(const char *state_dir,
                                            char *error_out,
                                            size_t error_out_size) {
    if (!catalog_requested_) {
        const OperationAdmission admission =
            loader_.request(state_dir, next_generation_);
        if (admission == OperationAdmission::Busy) {
            return StorageExportInventoryLoadResult::Waiting;
        }
        if (admission != OperationAdmission::Accepted) {
            copy_cstr(error_out, error_out_size, "export_inventory_rejected");
            return StorageExportInventoryLoadResult::Error;
        }
        advance_generation();
        catalog_requested_ = true;
    }

    const StorageExportInventoryLoadResult result =
        loader_.poll(error_out, error_out_size);
    if (result != StorageExportInventoryLoadResult::Ready) return result;

    catalog_ = loader_.snapshot();
    if (catalog_) return StorageExportInventoryLoadResult::Ready;

    copy_cstr(error_out, error_out_size, "export_inventory_missing");
    return StorageExportInventoryLoadResult::Error;
}

StorageExportDayRequestResult
StorageExportInventorySession::request_datalog_day(
    const char *day,
    StorageExportPlanner &planner,
    char *error_out,
    size_t error_out_size) {
    if (!storage_export_is_datalog_day_name(day)) {
        copy_cstr(error_out, error_out_size, "export_day_missing");
        return StorageExportDayRequestResult::Error;
    }

    if (datalog_day_ &&
        strcmp(datalog_day_->loaded_datalog_day(), day) == 0) {
        char pending_day[9] = {};
        if (planner.pending_datalog_day_inventory(pending_day,
                                                  sizeof(pending_day)) &&
            (strcmp(pending_day, day) != 0 ||
             !planner.provide_datalog_day_inventory(
                 datalog_day_, error_out, error_out_size))) {
            if (!error_out || !error_out[0]) {
                copy_cstr(error_out,
                          error_out_size,
                          "planner_day_inventory_failed");
            }
            return StorageExportDayRequestResult::Error;
        }
        return StorageExportDayRequestResult::Ready;
    }

    copy_cstr(requested_day_, sizeof(requested_day_), day);
    day_requested_ = false;
    return StorageExportDayRequestResult::Queued;
}

StorageExportInventoryLoadResult
StorageExportInventorySession::poll_datalog_day(
    StorageExportPlanner &planner,
    char *error_out,
    size_t error_out_size) {
    if (!requested_day_[0]) {
        copy_cstr(error_out, error_out_size, "export_day_missing");
        return StorageExportInventoryLoadResult::Error;
    }

    if (!day_requested_) {
        const OperationAdmission admission = loader_.request_datalog_day(
            requested_day_, next_generation_);
        if (admission == OperationAdmission::Busy) {
            return StorageExportInventoryLoadResult::Waiting;
        }
        if (admission != OperationAdmission::Accepted) {
            copy_cstr(error_out,
                      error_out_size,
                      "export_day_inventory_rejected");
            return StorageExportInventoryLoadResult::Error;
        }
        advance_generation();
        day_requested_ = true;
    }

    const StorageExportInventoryLoadResult result =
        loader_.poll(error_out, error_out_size);
    if (result != StorageExportInventoryLoadResult::Ready) return result;

    datalog_day_ = loader_.snapshot();
    if (!datalog_day_ ||
        strcmp(datalog_day_->loaded_datalog_day(), requested_day_) != 0) {
        copy_cstr(error_out,
                  error_out_size,
                  "export_day_inventory_missing");
        return StorageExportInventoryLoadResult::Error;
    }

    char pending_day[9] = {};
    if (planner.pending_datalog_day_inventory(pending_day,
                                              sizeof(pending_day)) &&
        (strcmp(pending_day, requested_day_) != 0 ||
         !planner.provide_datalog_day_inventory(
             datalog_day_, error_out, error_out_size))) {
        if (!error_out || !error_out[0]) {
            copy_cstr(error_out,
                      error_out_size,
                      "planner_day_inventory_failed");
        }
        return StorageExportInventoryLoadResult::Error;
    }

    day_requested_ = false;
    requested_day_[0] = '\0';
    return StorageExportInventoryLoadResult::Ready;
}

const StorageExportInventoryView *
StorageExportInventorySession::inventory_for_state_path(
    const char *state_dir,
    const char *state_path) const {
    char day[9] = {};
    if (!storage_export_state_path_datalog_day(state_dir,
                                               state_path,
                                               day,
                                               sizeof(day))) {
        return catalog_.get();
    }
    if (!datalog_day_ ||
        strcmp(datalog_day_->loaded_datalog_day(), day) != 0) {
        return nullptr;
    }
    return datalog_day_.get();
}

}  // namespace aircannect
