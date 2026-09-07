#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "display_report_summary.h"
#include "edf_session_metadata.h"
#include "night_catalog_refresh_service.h"
#include "night_catalog_store_service.h"
#include "report_engine.h"
#include "report_signal_store.h"
#include "report_signal_store_catalog.h"
#include "report_signal_store_catalog_service.h"
#include "report_summary_acquisition.h"
#include "runtime_snapshots.h"

namespace aircannect {

class StorageStatusPort;

enum class ReportTaskState : uint8_t {
    Stopped,
    LoadingCatalog,
    Idle,
    RefreshingCatalog,
    Queued,
    Building,
    Publishing,
};

enum class ReportTaskCondition : uint8_t {
    Stopped,
    Working,
    Waiting,
    Complete,
    Failed,
};

enum class ReportTaskOperation : uint8_t {
    None,
    LoadingCatalog,
    LoadingStoreCatalog,
    RefreshingCatalog,
    CheckingSpools,
    Building,
    Publishing,
    SavingCatalog,
};

enum class ReportTaskWaitReason : uint8_t {
    None,
    Startup,
    Queue,
    Catalog,
    Retry,
    Therapy,
    RealtimeStream,
    ForegroundRequest,
    Ota,
    Export,
    As11Unavailable,
};

struct ReportTaskOperationalSnapshot {
    ReportTaskCondition condition = ReportTaskCondition::Stopped;
    ReportTaskOperation operation = ReportTaskOperation::None;
    ReportTaskWaitReason wait_reason = ReportTaskWaitReason::None;
    SleepDayId sleep_day;
    size_t catalog_nights = 0;
    size_t materialized_nights = 0;
    uint32_t retry_in_ms = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

struct ReportTaskControlSnapshot {
    bool initialized = false;
    bool task_started = false;
    ReportTaskState state = ReportTaskState::Stopped;
    uint32_t catalog_generation = 0;
    uint32_t durable_catalog_generation = 0;
    bool foreground_active = false;
    bool background_active = false;
    bool post_therapy_settle_pending = false;
    NightCatalogRefreshState catalog_refresh_state =
        NightCatalogRefreshState::Idle;
    uint32_t catalog_refresh_generation = 0;
    bool catalog_refresh_retryable = false;
};

struct ReportTaskDiagnosticSnapshot {
    bool task_started = false;
    ReportTaskState state = ReportTaskState::Stopped;
    size_t commands_queued = 0;
    size_t catalog_nights = 0;
    size_t materialized_nights = 0;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;
    uint32_t catalog_generation = 0;
    uint32_t durable_catalog_generation = 0;
    bool foreground_active = false;
    bool background_active = false;
    bool background_suspended = false;

    ReportEngineState engine_state = ReportEngineState::Idle;
    size_t engine_queued = 0;
    SleepDayId engine_sleep_day;
    ReportExecutorState executor_state = ReportExecutorState::Idle;
    size_t executor_operation_index = 0;
    size_t executor_operation_count = 0;
    uint32_t executor_record_index = 0;
    uint32_t executor_record_count = 0;
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

    ReportSignalStoreCatalogLoadState store_catalog_state =
        ReportSignalStoreCatalogLoadState::Idle;
    size_t store_catalog_checked = 0;
    size_t store_catalog_loaded = 0;
    size_t store_catalog_skipped = 0;
    char store_catalog_error[AC_STORAGE_ERROR_MAX] = {};
};

enum class ReportStoreQueryState : uint8_t {
    Unavailable,
    CatalogPending,
    NightMissing,
    StorePending,
    TrackMissing,
    InvalidRange,
    Ready,
};

struct ReportNightQuery {
    ReportStoreQueryState state = ReportStoreQueryState::Unavailable;
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    std::shared_ptr<const LargeByteBuffer> metadata;
};

struct ReportSignalRangeQuery {
    ReportStoreQueryState state = ReportStoreQueryState::Unavailable;
    ReportSignalStoreTrack track;
    ReportSignalStoreLevel level = ReportSignalStoreLevel::Raw;
    int64_t first_block_start_ms = 0;
    size_t block_count = 0;
    ReportSignalStorePlaneRange range;
    uint64_t file_size = 0;
    char path[AC_STORAGE_PATH_MAX] = {};
};

struct ReportEventFileQuery {
    ReportStoreQueryState state = ReportStoreQueryState::Unavailable;
    SleepDayId sleep_day;
    SourceRevision source_revision;
    uint32_t generation = 0;
    uint64_t file_size = 0;
    uint32_t event_count = 0;
    char path[AC_STORAGE_PATH_MAX] = {};
};

struct ReportNightFailureStatus {
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint32_t retry_after_ms = 0;
    bool retryable = true;

    bool valid() const { return error[0] != '\0'; }
};

struct ReportRebuildStatus {
    uint32_t generation = 0;
    bool active = false;
    SleepDayId first_day;
    SleepDayId last_day;
    uint32_t completed = 0;
    uint32_t failed = 0;
    ReportEngineCompletion last_completion;
};

// Owns source discovery and v9 materialization on one low-priority task.
// Public methods enqueue work or read immutable metadata snapshots; HTTP file
// transfer remains owned by ReportHttpController and StorageStreamPort.
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
               StorageRangeWritePort &range_write_port,
               StorageStatusPort &status_port);

    OperationAdmission request_night(
        SleepDayId sleep_day,
        ReportRequestPriority priority,
        uint32_t generation,
        bool force_rebuild = false);
    OperationAdmission request_rebuild(SleepDayId first_day,
                                       SleepDayId last_day,
                                       uint32_t generation);
    ReportRebuildStatus rebuild_status() const;
    OperationAdmission publish_session_ended(
        uint32_t sessions_ended,
        const NightCatalogRefreshTarget &target = {});
    OperationAdmission publish_timezone_change(
        uint32_t revision,
        bool offset_valid,
        int32_t offset_minutes);
    void publish_activity(const ActivitySnapshot &activity);
    void publish_capture_session(const EdfSessionMetadata &metadata);

    ReportTaskControlSnapshot control_snapshot() const;
    ReportTaskOperationalSnapshot operational_snapshot() const;
    ReportTaskDiagnosticSnapshot diagnostic_snapshot() const;
    ReportEngineCompletion last_completion() const;
    std::shared_ptr<const NightCatalog> catalog_snapshot() const;
    std::shared_ptr<const ReportSignalStoreCatalog>
        store_catalog_snapshot() const;
    DisplayReportSummary display_summary_snapshot() const;

    ReportNightQuery query_night(SleepDayId sleep_day) const;
    ReportSignalRangeQuery query_signal(
        SleepDayId sleep_day,
        size_t metadata_track_index,
        int64_t first_block_start_ms,
        size_t block_count,
        ReportSignalStoreLevel level) const;
    ReportEventFileQuery query_events(SleepDayId sleep_day) const;
    bool night_failure(SleepDayId sleep_day,
                       ReportNightFailureStatus &failure,
                       uint32_t lock_timeout_ms = 20) const;

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
