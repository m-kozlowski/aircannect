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
#include "storage_atomic_write_port.h"
#include "storage_export_inventory.h"
#include "storage_export_plan.h"
#include "storage_export_planner.h"
#include "storage_export_state_batch.h"
#include "storage_file_client.h"
#include "storage_path.h"
#include "storage_path_port.h"
#include "storage_smb_client.h"
#include "storage_stream_reader.h"

namespace aircannect {

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

class StorageSyncEngine {
public:
    // lifecycle
    void begin(const SmbExportConfig &config,
               StorageScanPort &scan_port,
               StorageReadPort &read_port,
               StorageStreamPort &stream_port,
               StorageAtomicWritePort &write_port,
               StoragePathPort &path_port);

    // export task
    ExportStep step();

    // configuration/gates
    void configure(const SmbExportConfig &config);
    void set_network_available(bool available);
    void set_runtime_blocked(bool blocked);
    void defer_idle_work_until(uint32_t until_ms);

    // sync requests
    bool request_manual_sync();
    bool request_startup_check();
    bool request_verify_recent();
    bool request_post_therapy_sync();

    // status
    StorageSyncStatus status() const;
    StorageSyncRuntimeStatus runtime_status() const;

private:
    enum class WorkPhase : uint8_t {
        Idle,
        LoadMetadata,
        LoadInventory,
        Connect,
        VerifyLatestStart,
        VerifyLatestFile,
        VerifyLatestRemote,
        VerifyLatestInvalidate,
        NextFile,
        ResolveRemoteFile,
        EnsureRemoteDir,
        OpenLocal,
        OpenRemote,
        UploadChunk,
        CloseRemote,
        ValidateLocal,
        FlushState,
        WriteDoneMarker,
        Finish,
    };

    enum class FileCompletion : uint8_t {
        None,
        Uploaded,
        Skipped,
    };

    enum class RunKind : uint8_t {
        Manual,
        PostTherapy,
        StartupCheck,
        StartupSync,
        VerifyRecent,
        Retry,
    };

    struct CurrentFile {
        char path[AC_STORAGE_PATH_MAX] = {};
        char remote_path[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
        char remote_dir[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
        char state_path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
        uint64_t size = 0;
        uint64_t mtime = 0;
        uint64_t offset = 0;
        bool local_state_complete = false;
        bool batch_state = false;
        FileCompletion completion = FileCompletion::None;
        StorageStreamReader local;
    };

    struct LatestVerify {
        char day_path[AC_STORAGE_PATH_MAX] = {};
        char state_path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
        char current_path[AC_STORAGE_PATH_MAX] = {};
        char remote_path[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
        uint64_t current_size = 0;
        size_t source_index = 0;
        bool active = false;
        bool invalidate_state = false;
    };

    struct BlockingResult {
        uint32_t operation_generation = 0;
        bool ok = false;
        int transferred = 0;
        StorageSmbRemoteStat remote;
        char error[AC_STORAGE_ERROR_MAX] = {};
    };

    // locking/config
    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    void apply_config_locked(const SmbExportConfig &config);
    void apply_pending_config_locked();
    bool config_matches_locked(const SmbExportConfig &config) const;
    static bool snapshot_configured(const SmbExportConfig &config);
    static const char *work_phase_name(WorkPhase phase);
    static const char *run_kind_reason(RunKind kind);
    static bool run_kind_is_verify(RunKind kind);
    static bool run_kind_is_reconcile(RunKind kind);
    bool request_sync_with_kind(RunKind kind, const char *label);

    // run scheduling
    bool queue_post_therapy_locked(uint32_t now_ms);
    void queue_deferred_post_therapy_locked(uint32_t now_ms);
    void reset_run_locked(bool keep_status);
    bool prepare_step_locked(uint32_t now_ms, ExportStep &result);
    void queue_retry_locked(uint32_t now_ms);
    void queue_reconcile_if_due_locked(uint32_t now_ms);
    ExportStep step_work_phase_locked();
    static bool phase_has_blocking_io(WorkPhase phase);
    void execute_blocking_phase(WorkPhase phase, BlockingResult &result);
    ExportStep publish_blocking_phase_locked(
        WorkPhase phase,
        const BlockingResult &result);

    // SMB connection and transfer phases
    ExportStep step_verify_latest_start_locked(char *error_out,
                                               size_t error_out_size);
    ExportStep step_verify_latest_file_locked(char *error_out,
                                              size_t error_out_size);
    ExportStep publish_verify_latest_remote_locked(
        const StorageSmbRemoteStat &remote);
    ExportStep step_verify_latest_invalidate_locked();
    ExportStep step_validate_local_locked();
    ExportStep step_flush_state_locked();
    ExportStep step_write_done_marker_locked();

    // endpoint/state metadata
    bool build_endpoint_state_dir_locked(const SmbExportConfig &config,
                                         char *out,
                                         size_t out_size,
                                         uint32_t *hash_out = nullptr) const;
    bool begin_run_locked();
    ExportStep step_load_metadata_locked();
    ExportStep step_load_inventory_locked();
    void finish_run_locked();
    void preempt_run_locked();
    void fail_locked(const char *error);
    void clear_result_metadata_locked();
    void parse_result_metadata_locked(char *buffer);
    bool queue_result_metadata_save_locked();
    bool service_result_metadata_save_locked(ExportStep &result);
    void close_latest_verify_locked();
    bool begin_latest_verify_locked(char *error_out, size_t error_out_size);
    bool latest_verify_file_step_locked(char *error_out,
                                        size_t error_out_size);

    // export planning
    bool begin_export_planner_locked(char *error_out,
                                     size_t error_out_size);
    bool next_file_locked();
    bool plan_file_locked(const StorageExportPlannerItem &item);

    // local state files
    bool build_state_path_locked(const char *path,
                                 char *out,
                                 size_t out_size) const;
    bool queue_current_state_locked();
    void commit_pending_file_counts_locked();
    bool schedule_completed_datalog_day_locked(const char *day);
    bool prepare_state_file_locked();
    bool prepare_done_marker_locked();
    uint32_t next_storage_generation_locked();

    // path helpers and upload resources
    bool remote_parent_dir_locked(const char *remote_path,
                                  char *out,
                                  size_t out_size) const;
    bool prepare_upload_buffer_locked();
    void release_upload_buffer_locked();
    BackgroundOperationControl operation_control() const;
    void request_operation_abort();
    static bool operation_abort_cb(void *ctx);
    void close_local_locked(bool complete = false);
    void clear_current_file_locked();
    void publish_runtime_locked();

    // synchronization/status
    mutable SemaphoreHandle_t lock_ = nullptr;
    SmbExportConfig config_;
    SmbExportConfig pending_config_;
    bool pending_config_valid_ = false;
    StorageSyncStatus status_;
    uint32_t next_config_generation_ = 1;
    WorkPhase phase_ = WorkPhase::Idle;
    StorageSmbClient smb_;
    std::atomic<bool> network_available_{false};
    std::atomic<bool> runtime_blocked_{false};
    std::atomic<bool> abort_requested_{false};
    std::atomic<uint32_t> operation_generation_{1};
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
    StorageExportInventoryLoader inventory_loader_;
    StorageStreamPort *stream_port_ = nullptr;
    std::shared_ptr<const StorageExportInventory> export_inventory_;
    StorageExportPlanner export_planner_;
    StorageExportStateBatch state_batch_;
    StorageFileClient state_io_;
    StorageFileClient metadata_io_;
    CurrentFile current_file_;
    LatestVerify latest_verify_;
    uint8_t *upload_buffer_ = nullptr;
    size_t upload_buffer_size_ = 0;
    char ensured_remote_dir_[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
    char state_dir_[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    char pending_state_path_[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    char pending_done_day_[9] = {};
    std::shared_ptr<const LargeByteBuffer> pending_state_bytes_;
    uint32_t pending_state_uploaded_ = 0;
    uint32_t pending_state_skipped_ = 0;

    // durable result metadata
    char pending_metadata_path_[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    std::shared_ptr<const LargeByteBuffer> pending_metadata_bytes_;
    bool metadata_loaded_ = false;
    bool metadata_save_pending_ = false;

    // generations/retries
    uint32_t endpoint_hash_ = 0;
    uint32_t next_inventory_generation_ = 1;
    uint32_t next_storage_generation_ = 1;
    bool inventory_requested_ = false;
    uint32_t retry_due_ms_ = 0;
    uint8_t retry_attempt_ = 0;
};

}  // namespace aircannect
