#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <stdint.h>

#include "app_config.h"
#include "background_worker.h"
#include "storage_export_plan.h"
#include "storage_export_planner.h"
#include "storage_path.h"
#include "storage_smb_client.h"

namespace aircannect {

static constexpr size_t AC_STORAGE_SYNC_ENDPOINT_MAX = 161;
static constexpr size_t AC_STORAGE_SYNC_USER_MAX = 65;
static constexpr size_t AC_STORAGE_SYNC_PASSWORD_MAX = 129;
static constexpr size_t AC_STORAGE_SYNC_REASON_MAX = 24;
static constexpr size_t AC_STORAGE_SYNC_STATE_PATH_MAX = AC_STORAGE_PATH_MAX;
static constexpr size_t AC_STORAGE_SYNC_UPLOAD_CHUNK = 16 * 1024;

enum class StorageSyncState : uint8_t {
    Disabled,
    Idle,
    Pending,
    Working,
    Error,
};

const char *storage_sync_state_name(StorageSyncState state);

struct StorageSyncConfigStatus {
    bool enabled = false;
    bool configured = false;
    bool endpoint_set = false;
    bool user_set = false;
    bool password_set = false;
    bool network_available = false;
    uint32_t config_generation = 0;
};

struct StorageSyncResultStatus {
    uint64_t last_sync_epoch = 0;
    uint32_t last_sync_files_seen = 0;
    uint32_t last_sync_files_uploaded = 0;
    uint32_t last_sync_files_skipped = 0;
    uint32_t last_sync_files_failed = 0;
    uint64_t last_sync_bytes_uploaded = 0;
    uint64_t last_verify_epoch = 0;
    uint32_t last_verify_files_seen = 0;
    uint64_t last_reconcile_epoch = 0;
    uint32_t last_reconcile_files_seen = 0;
    uint64_t last_failure_epoch = 0;
    char last_failure_error[AC_STORAGE_ERROR_MAX] = {};
};

struct StorageSyncPersistentStatus
    : StorageSyncConfigStatus,
      StorageSyncResultStatus {};

struct StorageSyncTransientStatus {
    StorageSyncState state = StorageSyncState::Disabled;
    bool pending = false;
    bool last_run_verify = false;
    bool last_run_reconcile = false;
    char pending_reason[AC_STORAGE_SYNC_REASON_MAX] = {};
    char last_error[AC_STORAGE_ERROR_MAX] = {};
    char current_path[AC_STORAGE_PATH_MAX] = {};
    uint32_t files_seen = 0;
    uint32_t files_uploaded = 0;
    uint32_t files_skipped = 0;
    uint32_t files_failed = 0;
    uint64_t bytes_uploaded = 0;
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
    uint32_t retry_due_ms = 0;
    uint8_t retry_attempt = 0;
};

struct StorageSyncStatus
    : StorageSyncPersistentStatus,
      StorageSyncTransientStatus {};

struct StorageSyncRuntimeStatus {
    StorageSyncState state = StorageSyncState::Disabled;
    bool pending = false;
    bool enabled = false;
    bool configured = false;
    bool network_available = false;

    bool active() const {
        return state == StorageSyncState::Working ||
               ((state == StorageSyncState::Pending || pending) &&
                network_available);
    }
};

class StorageSyncJob : public BackgroundJob {
public:
    // lifecycle
    void begin(const AppConfigData &config);

    // background worker
    const char *name() const override { return "storage_sync"; }
    JobStep step() override;
    void on_preempt() override;

    // configuration/gates
    void configure(const AppConfigData &config);
    void refresh_config(const AppConfigData &config, uint32_t now_ms);
    void set_network_available(bool available);
    void set_runtime_blocked(bool blocked);
    void defer_idle_work_until(uint32_t until_ms);

    // sync requests
    bool request_manual_sync();
    bool request_verify_recent();
    bool request_post_therapy_sync();

    // status
    StorageSyncStatus status() const;
    StorageSyncRuntimeStatus runtime_status() const;
    bool active() const;

private:
    enum class WorkPhase : uint8_t {
        Idle,
        Connect,
        VerifyLatestStart,
        VerifyLatestFile,
        VerifyLatestInvalidate,
        NextFile,
        EnsureRemoteDir,
        OpenLocal,
        OpenRemote,
        UploadChunk,
        CloseRemote,
        MarkState,
        Finish,
    };

    enum class StateWriteMode : uint8_t {
        Append,
        Replace,
    };

    enum class RunKind : uint8_t {
        Manual,
        PostTherapy,
        StartupCheck,
        StartupSync,
        VerifyRecent,
        Retry,
    };

    struct ConfigSnapshot {
        bool enabled = false;
        char endpoint[AC_STORAGE_SYNC_ENDPOINT_MAX] = {};
        char user[AC_STORAGE_SYNC_USER_MAX] = {};
        char password[AC_STORAGE_SYNC_PASSWORD_MAX] = {};
    };

    struct CurrentFile {
        char path[AC_STORAGE_PATH_MAX] = {};
        char remote_path[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
        char remote_dir[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
        char state_path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint64_t offset = 0;
        StateWriteMode state_write_mode = StateWriteMode::Append;
        bool local_open = false;
        File local;
    };

    struct LatestVerify {
        char day_path[AC_STORAGE_PATH_MAX] = {};
        char state_path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
        bool opened = false;
        bool invalidate_state = false;
        File dir;
    };

    // locking/config
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void apply_config_locked(const ConfigSnapshot &config);
    bool config_matches_locked(const ConfigSnapshot &config) const;
    static ConfigSnapshot make_config_snapshot(const AppConfigData &config);
    static bool snapshot_configured(const ConfigSnapshot &config);
    static void copy_string(char *dst, size_t dst_size, const String &src);
    static const char *work_phase_name(WorkPhase phase);
    static const char *run_kind_reason(RunKind kind);
    static bool run_kind_is_verify(RunKind kind);
    static bool run_kind_is_reconcile(RunKind kind);
    bool request_sync_with_kind(RunKind kind, const char *label);

    // run scheduling
    bool queue_post_therapy_locked(uint32_t now_ms);
    void queue_deferred_post_therapy_locked(uint32_t now_ms);
    void reset_run_locked(bool keep_status);
    bool prepare_step_locked(uint32_t now_ms, JobStep &result);
    void queue_retry_locked(uint32_t now_ms);
    void queue_reconcile_if_due_locked(uint32_t now_ms);
    JobStep step_work_phase_locked();

    // SMB connection and transfer phases
    JobStep step_connect_locked(char *error_out, size_t error_out_size);
    JobStep step_verify_latest_start_locked(char *error_out,
                                            size_t error_out_size);
    JobStep step_verify_latest_file_locked(char *error_out,
                                           size_t error_out_size);
    JobStep step_verify_latest_invalidate_locked(char *error_out,
                                                 size_t error_out_size);
    JobStep step_ensure_remote_dir_locked(char *error_out,
                                          size_t error_out_size);
    JobStep step_open_local_locked();
    JobStep step_open_remote_locked(char *error_out, size_t error_out_size);
    JobStep step_upload_chunk_locked(char *error_out, size_t error_out_size);
    JobStep step_close_remote_locked(char *error_out, size_t error_out_size);
    JobStep step_mark_state_locked();

    // endpoint/state metadata
    bool build_endpoint_state_dir_locked(const ConfigSnapshot &config,
                                         char *out,
                                         size_t out_size,
                                         uint32_t *hash_out = nullptr) const;
    bool begin_run_locked();
    void finish_run_locked();
    void preempt_run_locked();
    void fail_locked(const char *error);
    void clear_result_metadata_locked();
    bool load_result_metadata_locked();
    bool save_result_metadata_locked();
    bool verify_endpoint_base_locked(char *error_out, size_t error_out_size);
    bool latest_datalog_day_locked(char *out,
                                   size_t out_size,
                                   char *error_out,
                                   size_t error_out_size) const;
    void close_latest_verify_locked();
    bool begin_latest_verify_locked(char *error_out, size_t error_out_size);
    bool latest_verify_file_step_locked(char *error_out,
                                        size_t error_out_size);
    bool invalidate_latest_state_locked(char *error_out,
                                        size_t error_out_size);

    // export planning
    bool begin_export_planner_locked(char *error_out,
                                     size_t error_out_size);
    bool next_file_locked();
    bool plan_file_locked(const StorageExportPlannerItem &item);

    // local state files
    bool ensure_state_dir_locked();
    bool build_state_path_locked(const char *path,
                                 char *out,
                                 size_t out_size,
                                 StateWriteMode *write_mode = nullptr) const;
    bool state_contains_locked(const char *state_path,
                               const char *path,
                               uint64_t size,
                               uint64_t mtime);
    bool write_state_locked(const char *state_path,
                            const char *path,
                            uint64_t size,
                            uint64_t mtime,
                            StateWriteMode mode);

    // path helpers and upload resources
    bool remote_parent_dir_locked(const char *remote_path,
                                  char *out,
                                  size_t out_size) const;
    bool datalog_day_name_from_path(const char *path,
                                    char *out,
                                    size_t out_size) const;
    void refresh_latest_datalog_day_name_locked();
    void maybe_mark_completed_datalog_day_locked(const char *day);
    bool prepare_upload_buffer_locked();
    void release_upload_buffer_locked();
    BackgroundOperationControl operation_control() const;
    static bool operation_abort_cb(void *ctx);
    void close_local_locked();
    void clear_current_file_locked();
    void publish_runtime_locked();

    // synchronization/status
    mutable SemaphoreHandle_t lock_ = nullptr;
    ConfigSnapshot config_;
    StorageSyncStatus status_;
    uint32_t next_config_generation_ = 1;
    uint32_t last_config_check_ms_ = 0;
    WorkPhase phase_ = WorkPhase::Idle;
    StorageSmbClient smb_;
    std::atomic<bool> network_available_{false};
    std::atomic<bool> runtime_blocked_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<uint8_t> runtime_state_{
        static_cast<uint8_t>(StorageSyncState::Disabled)};
    std::atomic<bool> runtime_pending_{false};
    std::atomic<bool> runtime_enabled_{false};
    std::atomic<bool> runtime_configured_{false};
    std::atomic<uint32_t> idle_defer_until_ms_{0};
    std::atomic<bool> post_therapy_requested_{false};
    RunKind pending_run_kind_ = RunKind::Manual;
    RunKind current_run_kind_ = RunKind::Manual;
    bool sync_after_verify_ = false;

    // active run
    StorageExportPlanner export_planner_;
    CurrentFile current_file_;
    LatestVerify latest_verify_;
    uint8_t *upload_buffer_ = nullptr;
    size_t upload_buffer_size_ = 0;
    StorageExportStateCache state_cache_;
    char ensured_remote_dir_[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
    char latest_datalog_day_[9] = {};
    char state_dir_[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    uint32_t endpoint_hash_ = 0;
    uint32_t retry_due_ms_ = 0;
    uint8_t retry_attempt_ = 0;
};

}  // namespace aircannect
