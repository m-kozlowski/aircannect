#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "night_catalog_refresh_service.h"
#include "night_catalog_store_service.h"
#include "report_artifact_payload_cache.h"
#include "report_artifact_payload_loader.h"
#include "report_artifact_index_refresh_service.h"
#include "report_engine.h"
#include "report_summary_acquisition.h"
#include "runtime_snapshots.h"
#include "storage_delete_port.h"

namespace aircannect {

enum class ReportTaskState : uint8_t {
    Stopped,
    LoadingCatalog,
    IndexingArtifacts,
    Idle,
    RefreshingCatalog,
    Queued,
    LookingUp,
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
    bool foreground_active = false;
    bool background_active = false;
    bool background_suspended = false;
    ReportSummaryAcquisitionStatus summary_acquisition;
    NightCatalogRefreshStatus catalog_refresh;
    NightCatalogStoreStatus catalog_store;
    ReportArtifactIndexRefreshStatus artifact_index_refresh;
    ReportArtifactPayloadCacheStatus payload_cache;
    ReportArtifactPayloadLoadStatus payload_load;
    ReportEngineStatus engine;
};

struct ReportTaskControlSnapshot {
    bool initialized = false;
    bool task_started = false;
    ReportTaskState state = ReportTaskState::Stopped;
    uint32_t catalog_generation = 0;
    bool foreground_active = false;
    bool background_active = false;
};

struct ReportTaskDiagnosticSnapshot {
    bool task_started = false;
    ReportTaskState state = ReportTaskState::Stopped;
    size_t commands_queued = 0;
    size_t catalog_nights = 0;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;
    uint32_t catalog_generation = 0;
    bool foreground_active = false;
    bool background_active = false;
    bool background_suspended = false;

    size_t payload_cache_entries = 0;
    size_t payload_cache_bytes = 0;
    uint32_t payload_cache_hits = 0;
    uint32_t payload_cache_misses = 0;
    uint32_t payload_cache_evictions = 0;
    ReportArtifactPayloadLoadState payload_load_state =
        ReportArtifactPayloadLoadState::Idle;
    size_t payload_load_bytes = 0;
    char payload_load_error[AC_STORAGE_ERROR_MAX] = {};

    ReportEngineState engine_state = ReportEngineState::Idle;
    size_t engine_queued = 0;
    char engine_error[AC_STORAGE_ERROR_MAX] = {};

    ReportFallbackAcquisitionState fallback_state =
        ReportFallbackAcquisitionState::Idle;
    ReportSourceId fallback_source = ReportSourceId::Summary;
    uint32_t fallback_sources_total = 0;
    uint32_t fallback_sources_completed = 0;
    uint32_t fallback_sections_added = 0;
    uint32_t fallback_unavailable_added = 0;
    char fallback_error[AC_STORAGE_ERROR_MAX] = {};

    NightCatalogRefreshState catalog_state =
        NightCatalogRefreshState::Idle;
    uint32_t catalog_files_seen = 0;
    uint32_t catalog_files_indexed = 0;
    uint32_t catalog_sessions = 0;
    char catalog_error[AC_STORAGE_ERROR_MAX] = {};
};

struct ReportArtifactFailureStatus {
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint32_t retry_after_ms = 0;

    bool valid() const { return error[0] != '\0'; }
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
               StorageScanPort &scan_port,
               ReportSpoolPort &spool_port,
               StorageDeletePort &delete_port);

    OperationAdmission request_artifact(
        const ReportArtifactKey &artifact,
        ReportRequestPriority priority,
        uint32_t generation);
    OperationAdmission request_payload_cache(
        const ReportArtifactKey &artifact,
        uint32_t generation);
    OperationAdmission request_catalog_refresh(
        bool current_offset_valid,
        int32_t current_offset_minutes,
        uint32_t generation);
    void publish_activity(const ActivitySnapshot &activity);

    ReportTaskControlSnapshot control_snapshot() const;
    ReportTaskDiagnosticSnapshot diagnostic_snapshot() const;
#ifndef ARDUINO
    ReportTaskStatus status() const;
#endif
    std::shared_ptr<const NightCatalog> catalog_snapshot() const;
    bool artifact_availability(const ReportArtifactKey &artifact,
                               ReportArtifactAvailability &availability) const;
    std::shared_ptr<const LargeByteBuffer> artifact_payload(
        const ReportArtifactDescriptor &artifact) const;
    bool artifact_failure(const ReportArtifactKey &artifact,
                          ReportArtifactFailureStatus &failure) const;

private:
    struct Runtime;
    friend struct ReportTaskTestAccess;

    static void task_entry(void *context);
    void run();
    bool step(uint32_t now_ms, size_t record_budget);
    void publish_catalog(std::shared_ptr<const NightCatalog> catalog,
                         uint32_t generation);

    Runtime *runtime_ = nullptr;
};

}  // namespace aircannect
