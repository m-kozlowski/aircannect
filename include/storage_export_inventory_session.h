#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "storage_export_inventory.h"
#include "storage_export_planner.h"

namespace aircannect {

enum class StorageExportDayRequestResult : uint8_t {
    Ready,
    Queued,
    Error,
};

class StorageExportInventorySession {
public:
    void begin(StorageScanPort &scan_port, StorageReadPort &read_port);
    void reset();

    StorageExportInventoryLoadResult poll_catalog(
        const char *state_dir, char *error_out, size_t error_out_size);
    StorageExportDayRequestResult request_datalog_day(
        const char *day,
        StorageExportPlanner &planner,
        char *error_out,
        size_t error_out_size);
    StorageExportInventoryLoadResult poll_datalog_day(
        StorageExportPlanner &planner,
        char *error_out,
        size_t error_out_size);

    const std::shared_ptr<const StorageExportInventory> &catalog() const {
        return catalog_;
    }
    const std::shared_ptr<const StorageExportInventory> &datalog_day() const {
        return datalog_day_;
    }
    const StorageExportInventoryView *inventory_for_state_path(
        const char *state_dir, const char *state_path) const;

private:
    void advance_generation();

    StorageExportInventoryLoader loader_;
    std::shared_ptr<const StorageExportInventory> catalog_;
    std::shared_ptr<const StorageExportInventory> datalog_day_;
    uint32_t next_generation_ = 1;
    bool catalog_requested_ = false;
    bool day_requested_ = false;
    char requested_day_[9] = {};
};

}  // namespace aircannect
