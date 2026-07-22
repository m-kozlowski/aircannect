#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog_refresh_service.h"
#include "night_catalog_store_service.h"
#include "report_engine.h"

namespace aircannect {

enum class ReportTaskState : uint8_t {
    Stopped,
    LoadingCatalog,
    Idle,
    RefreshingCatalog,
    Queued,
    Building,
    Publishing,
};

struct ReportTaskStatus {
    bool initialized = false;
    bool task_started = false;
    ReportTaskState state = ReportTaskState::Stopped;
    size_t commands_queued = 0;
    size_t catalog_nights = 0;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;
    uint32_t catalog_generation = 0;
    NightCatalogRefreshStatus catalog_refresh;
    NightCatalogStoreStatus catalog_store;
    ReportEngineStatus engine;
};

// Owns report state and runs it on one low-priority task. Public methods only
// enqueue commands or read immutable snapshots; they never execute report work
// on the caller's task.
class ReportTask {
public:
    ReportTask() = default;
    ~ReportTask();

    ReportTask(const ReportTask &) = delete;
    ReportTask &operator=(const ReportTask &) = delete;

    bool begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port,
               StorageScanPort &scan_port);

    OperationAdmission request_artifact(
        const ReportArtifactKey &artifact,
        ReportRequestPriority priority,
        uint32_t generation);
    OperationAdmission request_catalog_refresh(
        bool current_offset_valid,
        int32_t current_offset_minutes,
        uint32_t generation);
    OperationAdmission cancel_generation(uint32_t generation);

    ReportTaskStatus status() const;
    std::shared_ptr<const NightCatalog> catalog_snapshot() const;
    std::shared_ptr<const ReportArtifactBundle> take_published();

private:
    struct Runtime;
    friend struct ReportTaskTestAccess;

    static void task_entry(void *context);
    void run();
    bool step(uint32_t now_ms, size_t record_budget);

    Runtime *runtime_ = nullptr;
};

}  // namespace aircannect
