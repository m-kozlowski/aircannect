#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <memory>
#include <stdint.h>

#include "export_endpoint_config.h"
#include "export_step.h"
#include "large_byte_buffer.h"
#include "sleephq_client.h"
#include "sleephq_remote_file_cache.h"
#include "sleephq_sync_file.h"
#include "storage_atomic_write_port.h"
#include "storage_export_inventory.h"
#include "storage_export_plan.h"
#include "storage_export_planner.h"
#include "storage_export_state_batch.h"
#include "storage_file_client.h"
#include "storage_path.h"
#include "storage_path_port.h"

namespace aircannect {

static constexpr size_t AC_SLEEPHQ_SYNC_REASON_MAX = 24;
static constexpr size_t AC_SLEEPHQ_SYNC_STATE_PATH_MAX = AC_STORAGE_PATH_MAX;

enum class SleepHqSyncState : uint8_t {
    Disabled,
    Idle,
    Pending,
    Working,
    Error,
};

const char *sleephq_sync_state_name(SleepHqSyncState state);

struct SleepHqSyncStatus {
    SleepHqSyncState state = SleepHqSyncState::Disabled;
    bool configured = false;
    bool network_available = false;
    bool pending = false;
    char pending_reason[AC_SLEEPHQ_SYNC_REASON_MAX] = {};
    char last_error[AC_SLEEPHQ_ERROR_MAX] = {};
    char current_path[AC_STORAGE_PATH_MAX] = {};
    uint32_t config_generation = 0;
    uint32_t team_id = 0;
    uint32_t import_id = 0;
    char import_status[AC_SLEEPHQ_STATUS_MAX] = {};
    uint32_t files_seen = 0;
    uint32_t files_uploaded = 0;
    uint32_t files_skipped = 0;
    uint32_t files_failed = 0;
    uint64_t bytes_uploaded = 0;
    uint64_t last_check_epoch = 0;
    uint64_t last_sync_epoch = 0;
    uint32_t last_sync_files_seen = 0;
    uint32_t last_sync_files_uploaded = 0;
    uint32_t last_sync_files_failed = 0;
    uint64_t last_sync_bytes_uploaded = 0;
    uint64_t last_failure_epoch = 0;
    uint32_t retry_due_ms = 0;
    uint8_t retry_attempt = 0;
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
};

struct SleepHqSyncRuntimeStatus {
    SleepHqSyncState state = SleepHqSyncState::Disabled;
    bool pending = false;
    bool configured = false;
    bool network_available = false;
    uint32_t config_generation = 0;
    uint32_t team_id = 0;
    uint32_t completed_check_generation = 0;

    bool active() const {
        return state == SleepHqSyncState::Working ||
               ((state == SleepHqSyncState::Pending || pending) &&
                network_available);
    }

    bool check_complete() const {
        return config_generation != 0 &&
               completed_check_generation == config_generation;
    }
};

class SleepHqSyncEngine {
public:
    // lifecycle
    void begin(const SleepHqExportConfig &config,
               StorageScanPort &scan_port,
               StorageReadPort &read_port,
               StorageStreamPort &stream_port,
               StorageAtomicWritePort &write_port,
               StoragePathPort &path_port);

    // export task
    ExportStep step();

    // configuration/gates
    void configure(const SleepHqExportConfig &config);
    void set_network_available(bool available);
    void set_runtime_blocked(bool blocked);

    // sync requests
    bool request_check(const char *reason = "manual");
    bool request_sync(const char *reason = "manual");
    bool request_sync_day(const char *day, const char *reason = "manual_day");
    bool request_post_therapy_sync();

    // status
    SleepHqSyncStatus status() const;
    SleepHqSyncRuntimeStatus runtime_status() const;

private:
    enum class WorkPhase : uint8_t {
        Idle,
        Connect,
        LoadInventory,
        ReadIdentification,
        FindRemoteMachine,
        LoadInflight,
        NextFile,
        ResolveDatalogDay,
        ReadRebuildMarker,
        CreateImport,
        OpenLocal,
        HashLocalFile,
        ResolveRemoteFile,
        FetchRemoteFiles,
        UploadFile,
        ProcessImport,
        WaitImport,
        FetchImport,
        WriteInflight,
        ValidateStaged,
        FlushState,
        WriteRebuildMarker,
        WriteDoneMarker,
        RemoveInflight,
        Finish,
    };

    enum class RunKind : uint8_t {
        Check,
        Sync,
        PostTherapySync,
    };

    enum class InflightPhase : uint8_t {
        None,
        Uploading,
        Processing,
    };

    enum class InflightRemoveAction : uint8_t {
        None,
        ResumeExport,
        ResetBatch,
        Fail,
    };

    struct StagedFile {
        char path[AC_STORAGE_PATH_MAX] = {};
        char state_path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
        char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint32_t import_id = 0;
    };

    struct RemoteMachineDateCache {
        char day[9] = {};
        bool exists = false;
    };

    struct MachineListContext {
        const char *serial = nullptr;
        uint32_t machine_id = 0;
    };

    struct BlockingResult {
        uint32_t operation_generation = 0;
        bool ok = false;
        bool performed = false;
        bool has_more = false;
        bool retryable = false;
        bool remote_date_exists = false;
        size_t count = 0;
        uint32_t team_id = 0;
        uint32_t machine_id = 0;
        SleepHqImportInfo import;
        SleepHqUploadResult upload;
        SleepHqRemoteFileCache remote_files;
        char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
        char serial[AC_SLEEPHQ_SERIAL_MAX] = {};
        char error[AC_SLEEPHQ_ERROR_MAX] = {};
    };

    // locking/config
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    static bool snapshot_configured(const SleepHqExportConfig &config);
    static SleepHqConfig client_config_from_snapshot(
        const SleepHqExportConfig &config);
    static const char *inflight_phase_name(InflightPhase phase);
    static bool parse_inflight_phase(const char *text, InflightPhase &out);

    void apply_config_locked(const SleepHqExportConfig &config);
    void apply_pending_config_locked();
    bool config_matches_locked(const SleepHqExportConfig &config) const;
    bool request_locked(RunKind kind,
                        const char *reason,
                        const char *datalog_day = nullptr);

    // run lifecycle
    bool begin_run_locked(uint32_t now_ms);
    ExportStep step_load_inventory_locked();
    void queue_retry_locked(uint32_t now_ms);
    void reset_run_locked(bool keep_status);
    void finish_check_locked(uint32_t team_id);
    void finish_sync_locked();
    void fail_locked(const char *error);
    void clear_current_file_locked();
    void publish_runtime_locked();

    // export planning and import batches
    bool begin_export_planner_locked(char *error_out,
                                     size_t error_out_size);
    void reset_import_batch_locked();
    void complete_import_batch_reset_locked();
    ExportStep finish_import_or_sync_locked();
    bool next_file_locked();
    bool plan_file_locked(const StorageExportPlannerItem &item);
    bool build_endpoint_state_dir_locked(uint32_t team_id,
                                         char *out,
                                         size_t out_size) const;

    // durable sync state
    bool build_inflight_path_locked(char *out, size_t out_size) const;
    bool prepare_inflight_write_locked(InflightPhase phase,
                                       WorkPhase next_phase);
    bool build_inflight_bytes_locked(InflightPhase phase);
    ExportStep step_load_inflight_locked();
    ExportStep step_write_inflight_locked();
    ExportStep step_remove_inflight_locked();
    ExportStep step_validate_staged_locked();
    bool parse_inflight_line_locked(char *line);
    void reset_inflight_reader_locked();
    void complete_inflight_load_locked();
    bool begin_inflight_remove_locked(InflightRemoveAction action,
                                      const char *failure = nullptr);
    bool staged_contains_locked(const char *path,
                                uint64_t size,
                                uint64_t mtime) const;
    void note_completed_datalog_day_locked(const char *day);
    bool prepare_staged_state_locked();
    ExportStep step_flush_state_locked();
    ExportStep step_write_rebuild_marker_locked();
    ExportStep step_write_done_marker_locked();
    bool prepare_rebuild_marker_locked();
    bool prepare_done_marker_locked();
    void continue_after_state_flush_locked();
    uint32_t next_storage_generation_locked();
    bool reserve_staged_locked(size_t needed);
    bool add_staged_locked(const SleepHqUploadResult &upload);
    void clear_staged_locked();

    // remote file cache
    bool remote_file_cache_contains_locked() const;
    void clear_remote_files_locked();

    // remote machine/date reconcile
    bool reserve_remote_dates_locked(size_t needed);
    bool cache_remote_date_locked(const char *day, bool exists);
    bool cached_remote_date_exists_locked(const char *day,
                                          bool &exists) const;
    void clear_remote_dates_locked();
    bool build_datalog_rebuild_marker_path_locked(const char *day,
                                                  char *out,
                                                  size_t out_size) const;
    bool begin_remote_missing_datalog_day_locked(char *error,
                                                 size_t error_size);
    bool resolve_remote_missing_datalog_day_locked(bool marker_recent,
                                                   uint64_t marker_epoch,
                                                   char *error,
                                                   size_t error_size);
    ExportStep step_read_rebuild_marker_locked();
    bool read_local_machine_serial(
        char *out,
        size_t out_size,
        const BackgroundOperationControl &operation,
        char *error,
        size_t error_size);
    bool prepare_remote_reconcile_locked(char *error, size_t error_size);
    void note_remote_machine_missing_locked();

    // work phases
    ExportStep begin_export_work_locked();
    bool resolve_pending_datalog_day_locked(bool &needs_lookup,
                                            char *error,
                                            size_t error_size);
    BackgroundOperationControl operation_control(uint32_t timeout_ms) const;
    void request_operation_abort();

    ExportStep step_resolve_remote_file_locked();
    ExportStep step_wait_import_locked();
    ExportStep step_work_phase_locked();
    static bool phase_has_blocking_io(WorkPhase phase);
    void execute_blocking_phase(WorkPhase phase, BlockingResult &result);
    ExportStep publish_blocking_phase_locked(WorkPhase phase,
                                             BlockingResult &result);

    // client callbacks
    static bool operation_abort_cb(void *ctx);
    static bool remote_file_list_cb(void *ctx,
                                    const SleepHqRemoteFile &file);
    static bool remote_machine_list_cb(void *ctx,
                                       const SleepHqMachine &machine);

    // synchronization/status
    mutable SemaphoreHandle_t lock_ = nullptr;
    SleepHqSyncStatus status_;
    SleepHqExportConfig config_;
    SleepHqExportConfig pending_config_;
    bool pending_config_valid_ = false;
    WorkPhase phase_ = WorkPhase::Idle;
    RunKind pending_run_kind_ = RunKind::Check;
    RunKind current_run_kind_ = RunKind::Check;
    SleepHqClient client_;
    std::atomic<bool> network_available_{false};
    std::atomic<bool> runtime_blocked_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<uint32_t> operation_generation_{1};
    std::atomic<uint8_t> runtime_state_{
        static_cast<uint8_t>(SleepHqSyncState::Disabled)};
    std::atomic<bool> runtime_pending_{false};
    std::atomic<bool> runtime_configured_{false};
    std::atomic<uint32_t> runtime_config_generation_{0};
    std::atomic<uint32_t> runtime_team_id_{0};
    std::atomic<uint32_t> runtime_completed_check_generation_{0};

    // active export/import
    StorageExportInventoryLoader inventory_loader_;
    StorageStreamPort *stream_port_ = nullptr;
    std::shared_ptr<const StorageExportInventory> export_inventory_;
    StorageExportPlanner export_planner_;
    StorageExportStateBatch state_batch_;
    StorageFileClient state_io_;
    bool import_batch_active_ = false;
    SleepHqSyncFile current_file_;
    StagedFile *staged_ = nullptr;
    size_t staged_count_ = 0;
    size_t staged_capacity_ = 0;
    SleepHqRemoteFileCache remote_files_;
    uint32_t remote_file_next_page_ = 1;
    uint32_t remote_file_pages_loaded_ = 0;
    bool remote_file_cache_complete_ = false;

    // remote reconcile
    RemoteMachineDateCache *remote_dates_ = nullptr;
    size_t remote_date_count_ = 0;
    size_t remote_date_capacity_ = 0;
    uint32_t remote_machine_id_ = 0;
    uint32_t remote_machine_next_page_ = 1;
    uint32_t remote_machine_pages_loaded_ = 0;
    bool remote_reconcile_enabled_ = false;
    bool remote_reconcile_all_missing_ = false;
    char remote_serial_[AC_SLEEPHQ_SERIAL_MAX] = {};

    // day/state tracking
    char pending_rebuild_day_[9] = {};
    char pending_done_day_[9] = {};
    char pending_remote_day_[9] = {};
    bool pending_remote_day_local_complete_ = false;
    char pending_datalog_day_[9] = {};
    char current_datalog_day_filter_[9] = {};
    size_t staged_validation_index_ = 0;
    uint32_t import_process_started_ms_ = 0;
    uint32_t import_poll_due_ms_ = 0;
    uint32_t retry_due_ms_ = 0;
    uint8_t retry_attempt_ = 0;
    InflightPhase inflight_phase_ = InflightPhase::None;
    InflightPhase pending_inflight_phase_ = InflightPhase::None;
    InflightRemoveAction inflight_remove_action_ =
        InflightRemoveAction::None;
    WorkPhase storage_next_phase_ = WorkPhase::Idle;
    char storage_failure_[AC_SLEEPHQ_ERROR_MAX] = {};

    // prepared durable state
    char state_dir_[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
    char pending_state_path_[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
    std::shared_ptr<const LargeByteBuffer> pending_state_bytes_;
    StoragePreparedFile rebuild_marker_file_;
    StoragePreparedFile inflight_file_;
    size_t inflight_read_offset_ = 0;
    size_t inflight_line_length_ = 0;
    bool inflight_header_seen_ = false;
    bool inflight_parse_failed_ = false;
    char inflight_line_[AC_STORAGE_PATH_MAX * 2 + 160] = {};

    // storage generations
    uint32_t next_inventory_generation_ = 1;
    uint32_t next_storage_generation_ = 1;
    bool inventory_requested_ = false;
};

}  // namespace aircannect
