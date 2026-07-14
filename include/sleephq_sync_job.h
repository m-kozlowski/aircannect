#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <stdint.h>

#include "app_config.h"
#include "background_worker.h"
#include "sleephq_client.h"
#include "sleephq_remote_file_cache.h"
#include "storage_export_plan.h"
#include "storage_export_planner.h"
#include "storage_path.h"

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

class SleepHqSyncJob : public BackgroundJob {
public:
    // lifecycle
    void begin(const AppConfigData &config);

    // background worker
    const char *name() const override { return "sleephq_sync"; }
    JobStep step() override;
    void on_preempt() override;

    // configuration/gates
    void refresh_config(const AppConfigData &config, uint32_t now_ms);
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
    bool active() const;

private:
    enum class WorkPhase : uint8_t {
        Idle,
        Connect,
        FindRemoteMachine,
        NextFile,
        ResolveDatalogDay,
        CreateImport,
        OpenLocal,
        HashLocalFile,
        ResolveRemoteFile,
        FetchRemoteFiles,
        UploadFile,
        ProcessImport,
        WaitImport,
        FetchImport,
        MarkState,
        Finish,
    };

    enum class RunKind : uint8_t {
        Check,
        Sync,
        PostTherapySync,
    };

    enum class StateWriteMode : uint8_t {
        Append,
        Replace,
    };

    enum class InflightPhase : uint8_t {
        None,
        Uploading,
        Processing,
    };

    struct ConfigSnapshot {
        char client_id[AC_SLEEPHQ_SECRET_MAX] = {};
        char client_secret[AC_SLEEPHQ_SECRET_MAX] = {};
        char team_id[AC_SLEEPHQ_ID_MAX] = {};
        char device_id[AC_SLEEPHQ_ID_MAX] = {};
    };

    struct CurrentFile {
        char path[AC_STORAGE_PATH_MAX] = {};
        char sleep_path[AC_STORAGE_PATH_MAX] = {};
        char name[AC_STORAGE_NAME_MAX] = {};
        char state_path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint64_t offset = 0;
        StateWriteMode state_write_mode = StateWriteMode::Append;
        bool local_open = false;
        bool force_upload = false;
        bool attach_by_hash = false;
        char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
        File local;
    };

    struct StagedFile {
        char path[AC_STORAGE_PATH_MAX] = {};
        char state_path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
        char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint32_t import_id = 0;
        StateWriteMode state_write_mode = StateWriteMode::Append;
    };

    struct RemoteMachineDateCache {
        char day[9] = {};
        bool exists = false;
    };

    struct UploadContext {
        File *file = nullptr;
        std::atomic<bool> *abort_requested = nullptr;
        uint64_t offset = 0;
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
        File local;
        char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
        char error[AC_SLEEPHQ_ERROR_MAX] = {};
    };

    // locking/config
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    static ConfigSnapshot make_config_snapshot(const AppConfigData &config);
    static bool snapshot_configured(const ConfigSnapshot &config);
    static SleepHqConfig client_config_from_snapshot(
        const ConfigSnapshot &config);
    static void copy_string(char *dst, size_t dst_size, const String &src);
    static const char *inflight_phase_name(InflightPhase phase);
    static bool parse_inflight_phase(const char *text, InflightPhase &out);

    void apply_config_locked(const ConfigSnapshot &config);
    void apply_pending_config_locked();
    bool config_matches_locked(const ConfigSnapshot &config) const;
    bool request_locked(RunKind kind,
                        const char *reason,
                        const char *datalog_day = nullptr);

    // run lifecycle
    bool begin_run_locked(uint32_t now_ms);
    void queue_retry_locked(uint32_t now_ms);
    void reset_run_locked(bool keep_status);
    void finish_check_locked(uint32_t team_id);
    void finish_sync_locked();
    void fail_locked(const char *error);
    void close_local_locked();
    void clear_current_file_locked();
    void publish_runtime_locked();

    // export planning and import batches
    bool begin_export_planner_locked(char *error_out,
                                     size_t error_out_size);
    void reset_import_batch_locked();
    JobStep finish_import_or_sync_locked();
    bool next_file_locked();
    bool plan_file_locked(const StorageExportPlannerItem &item);
    bool build_endpoint_state_dir_locked(uint32_t team_id,
                                         char *out,
                                         size_t out_size) const;

    // durable sync state
    bool ensure_state_dir_locked();
    bool build_inflight_path_locked(char *out, size_t out_size) const;
    bool load_inflight_locked(InflightPhase &phase_out);
    bool write_inflight_locked(InflightPhase phase);
    void remove_inflight_locked();
    bool staged_contains_locked(const char *path,
                                uint64_t size,
                                uint64_t mtime) const;
    void refresh_latest_datalog_day_name_locked();
    void note_completed_datalog_day_locked(const char *day);
    void maybe_mark_completed_datalog_day_locked();
    bool write_state_locked(const StagedFile &file);
    bool reserve_staged_locked(size_t needed);
    bool add_staged_locked(const SleepHqUploadResult &upload);
    void clear_staged_locked();

    // remote file cache
    bool remote_file_cache_contains_locked(const CurrentFile &file) const;
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
    bool read_datalog_rebuild_marker_locked(const char *day,
                                            uint64_t &epoch) const;
    bool datalog_rebuild_marker_recent_locked(const char *day,
                                              uint64_t now_epoch,
                                              uint64_t &marker_epoch) const;
    bool mark_datalog_rebuild_attempt_locked(const char *day,
                                             uint64_t now_epoch);
    void maybe_mark_datalog_rebuild_success_locked();
    bool force_remote_missing_datalog_day_locked(const char *day,
                                                 bool local_complete,
                                                 bool &force_export);
    bool read_local_machine_serial_locked(char *out,
                                          size_t out_size,
                                          char *error,
                                          size_t error_size);
    bool prepare_remote_reconcile_locked(char *error, size_t error_size);
    void note_remote_machine_missing_locked();

    // work phases
    JobStep begin_export_work_locked();
    bool resolve_pending_datalog_day_locked(bool &needs_lookup,
                                            char *error,
                                            size_t error_size);
    bool build_sleep_path_locked(const char *local_path,
                                 char *path_out,
                                 size_t path_out_size,
                                 char *name_out,
                                 size_t name_out_size) const;
    bool current_file_matches_snapshot() const;
    bool compute_current_file_content_hash(char *out, size_t out_size);
    BackgroundOperationControl operation_control(uint32_t timeout_ms) const;
    void request_operation_abort();

    JobStep step_resolve_remote_file_locked();
    JobStep step_wait_import_locked();
    JobStep step_mark_state_locked(char *error, size_t error_size);
    JobStep step_work_phase_locked();
    static bool phase_has_blocking_io(WorkPhase phase);
    void execute_blocking_phase(WorkPhase phase, BlockingResult &result);
    JobStep publish_blocking_phase_locked(WorkPhase phase,
                                          BlockingResult &result);

    // client callbacks
    static bool upload_read_cb(void *ctx,
                               uint8_t *out,
                               size_t len,
                               size_t &read);
    static bool upload_reset_cb(void *ctx);
    static bool operation_abort_cb(void *ctx);
    static bool remote_file_list_cb(void *ctx,
                                    const SleepHqRemoteFile &file);
    static bool remote_machine_list_cb(void *ctx,
                                       const SleepHqMachine &machine);

    // synchronization/status
    mutable SemaphoreHandle_t lock_ = nullptr;
    SleepHqSyncStatus status_;
    ConfigSnapshot config_;
    ConfigSnapshot pending_config_;
    bool pending_config_valid_ = false;
    uint32_t last_config_check_ms_ = 0;
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
    StorageExportPlanner export_planner_;
    bool import_batch_active_ = false;
    CurrentFile current_file_;
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
    char latest_datalog_day_[9] = {};
    size_t mark_index_ = 0;
    uint32_t import_process_started_ms_ = 0;
    uint32_t import_poll_due_ms_ = 0;
    uint32_t retry_due_ms_ = 0;
    uint8_t retry_attempt_ = 0;
    InflightPhase inflight_phase_ = InflightPhase::None;
    StorageExportStateCache state_cache_;
    char state_dir_[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
};

}  // namespace aircannect
