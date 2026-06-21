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
    void begin(const AppConfigData &config);

    const char *name() const override { return "sleephq_sync"; }
    JobStep step() override;
    void on_preempt() override;

    void refresh_config(const AppConfigData &config, uint32_t now_ms);
    void set_network_available(bool available);
    void set_runtime_blocked(bool blocked);
    bool request_check(const char *reason = "manual");
    bool request_sync(const char *reason = "manual");
    bool request_post_therapy_sync();

    SleepHqSyncStatus status() const;
    SleepHqSyncRuntimeStatus runtime_status() const;
    bool active() const;

private:
    enum class WorkPhase : uint8_t {
        Idle,
        Connect,
        FindRemoteMachine,
        Check,
        NextFile,
        CreateImport,
        OpenLocal,
        ResolveRemoteFile,
        UploadFile,
        ProcessImport,
        WaitImport,
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

    struct RemoteFile {
        uint32_t id = 0;
        uint64_t size = 0;
        char name[AC_STORAGE_NAME_MAX] = {};
        char path[AC_STORAGE_PATH_MAX] = {};
        char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
    };

    struct RemoteMachineDateCache {
        char day[9] = {};
        bool exists = false;
    };

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
    bool config_matches_locked(const ConfigSnapshot &config) const;
    bool request_locked(RunKind kind, const char *reason);
    bool begin_run_locked(uint32_t now_ms);
    void queue_retry_locked(uint32_t now_ms);
    void reset_run_locked(bool keep_status);
    void finish_check_locked(uint32_t team_id);
    void finish_sync_locked();
    void fail_locked(const char *error);
    void close_local_locked();
    void clear_current_file_locked();
    void publish_runtime_locked();
    bool begin_export_planner_locked(char *error_out,
                                     size_t error_out_size);
    void reset_import_batch_locked();
    JobStep finish_import_or_sync_locked();
    bool next_file_locked();
    bool plan_file_locked(const StorageExportPlannerItem &item);
    bool build_endpoint_state_dir_locked(uint32_t team_id,
                                         char *out,
                                         size_t out_size) const;
    bool ensure_state_dir_locked();
    void note_state_written_locked(const char *state_path,
                                   const char *path,
                                   uint64_t size,
                                   uint64_t mtime,
                                   StateWriteMode mode);
    bool build_inflight_path_locked(char *out, size_t out_size) const;
    bool load_inflight_locked(InflightPhase &phase_out);
    bool write_inflight_locked(InflightPhase phase);
    void remove_inflight_locked();
    bool staged_contains_locked(const char *path,
                                uint64_t size,
                                uint64_t mtime) const;
    bool write_state_locked(const StagedFile &file);
    bool append_state_locked(const StagedFile &file);
    bool replace_state_locked(const StagedFile &file);
    bool reserve_staged_locked(size_t needed);
    bool add_staged_locked(const SleepHqUploadResult &upload);
    void clear_staged_locked();
    bool reserve_remote_files_locked(size_t needed);
    bool add_remote_file_locked(const SleepHqRemoteFile &file);
    bool remote_file_cache_contains_locked(const CurrentFile &file) const;
    bool fetch_next_remote_file_page_locked(char *error,
                                            size_t error_size);
    void clear_remote_files_locked();
    bool reserve_remote_dates_locked(size_t needed);
    bool cache_remote_date_locked(const char *day, bool exists);
    bool cached_remote_date_exists_locked(const char *day,
                                          bool &exists) const;
    void clear_remote_dates_locked();
    bool read_local_machine_serial_locked(char *out,
                                          size_t out_size,
                                          char *error,
                                          size_t error_size);
    bool prepare_remote_reconcile_locked(char *error, size_t error_size);
    bool note_remote_machine_locked(const SleepHqMachine &machine);
    void note_remote_machine_missing_locked();
    JobStep begin_export_work_locked();
    JobStep step_find_remote_machine_locked(char *error, size_t error_size);
    bool datalog_day_decision_locked(const char *day,
                                     bool local_complete,
                                     bool &force_export,
                                     char *error,
                                     size_t error_size);
    bool local_ensure_dir_locked(const char *path);
    bool build_sleep_path_locked(const char *local_path,
                                 char *path_out,
                                 size_t path_out_size,
                                 char *name_out,
                                 size_t name_out_size) const;
    bool current_file_matches_snapshot_locked() const;
    bool compute_current_file_content_hash_locked(char *out,
                                                  size_t out_size);

    JobStep step_connect_locked(char *error, size_t error_size);
    JobStep step_check_locked(char *error, size_t error_size);
    JobStep step_create_import_locked(char *error, size_t error_size);
    JobStep step_open_local_locked();
    JobStep step_resolve_remote_file_locked(char *error,
                                            size_t error_size);
    JobStep step_upload_file_locked(char *error, size_t error_size);
    JobStep step_process_import_locked(char *error, size_t error_size);
    JobStep step_wait_import_locked(char *error, size_t error_size);
    JobStep step_mark_state_locked(char *error, size_t error_size);
    JobStep step_work_phase_locked();

    static bool upload_read_cb(void *ctx, uint8_t *out,
                               size_t len, size_t &read);
    static bool upload_reset_cb(void *ctx);
    static bool upload_abort_cb(void *ctx);
    static bool remote_file_list_cb(void *ctx,
                                    const SleepHqRemoteFile &file);
    static bool remote_machine_list_cb(void *ctx,
                                       const SleepHqMachine &machine);
    static bool datalog_day_decision_cb(void *ctx,
                                        const char *day,
                                        bool local_complete,
                                        bool &force_export,
                                        char *error,
                                        size_t error_size);

    mutable SemaphoreHandle_t lock_ = nullptr;
    SleepHqSyncStatus status_;
    ConfigSnapshot config_;
    uint32_t last_config_check_ms_ = 0;
    WorkPhase phase_ = WorkPhase::Idle;
    RunKind pending_run_kind_ = RunKind::Check;
    RunKind current_run_kind_ = RunKind::Check;
    SleepHqClient client_;
    std::atomic<bool> network_available_{false};
    std::atomic<bool> runtime_blocked_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<uint8_t> runtime_state_{
        static_cast<uint8_t>(SleepHqSyncState::Disabled)};
    std::atomic<bool> runtime_pending_{false};
    std::atomic<bool> runtime_configured_{false};
    std::atomic<uint32_t> runtime_config_generation_{0};
    std::atomic<uint32_t> runtime_team_id_{0};
    std::atomic<uint32_t> runtime_completed_check_generation_{0};
    StorageExportPlanner export_planner_;
    bool import_batch_active_ = false;
    CurrentFile current_file_;
    StagedFile *staged_ = nullptr;
    size_t staged_count_ = 0;
    size_t staged_capacity_ = 0;
    RemoteFile *remote_files_ = nullptr;
    size_t remote_file_count_ = 0;
    size_t remote_file_capacity_ = 0;
    uint32_t remote_file_next_page_ = 1;
    uint32_t remote_file_pages_loaded_ = 0;
    bool remote_file_cache_complete_ = false;
    RemoteMachineDateCache *remote_dates_ = nullptr;
    size_t remote_date_count_ = 0;
    size_t remote_date_capacity_ = 0;
    uint32_t remote_machine_id_ = 0;
    uint32_t remote_machine_next_page_ = 1;
    uint32_t remote_machine_pages_loaded_ = 0;
    bool remote_reconcile_enabled_ = false;
    bool remote_reconcile_all_missing_ = false;
    char remote_serial_[AC_SLEEPHQ_SERIAL_MAX] = {};
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
