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
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
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

    SleepHqSyncStatus status() const;
    bool active() const;

private:
    enum class WorkPhase : uint8_t {
        Idle,
        Connect,
        Check,
        NextFile,
        CreateImport,
        OpenLocal,
        UploadFile,
        ProcessImport,
        WaitImport,
        MarkState,
        Finish,
    };

    enum class RunKind : uint8_t {
        Check,
        Sync,
    };

    enum class StateWriteMode : uint8_t {
        Append,
        Replace,
    };

    struct ConfigSnapshot {
        char client_id[AC_SLEEPHQ_SECRET_MAX] = {};
        char client_secret[AC_SLEEPHQ_SECRET_MAX] = {};
        char team_id[AC_SLEEPHQ_ID_MAX] = {};
        char device_id[AC_SLEEPHQ_ID_MAX] = {};
    };

    struct WalkFrame {
        char path[AC_STORAGE_PATH_MAX] = {};
        uint32_t next_index = 0;
        bool opened = false;
        File dir;
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

    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    static ConfigSnapshot make_config_snapshot(const AppConfigData &config);
    static bool snapshot_configured(const ConfigSnapshot &config);
    static SleepHqConfig client_config_from_snapshot(
        const ConfigSnapshot &config);
    static void copy_string(char *dst, size_t dst_size, const String &src);

    void apply_config_locked(const ConfigSnapshot &config);
    bool config_matches_locked(const ConfigSnapshot &config) const;
    bool request_locked(RunKind kind, const char *reason);
    bool begin_run_locked(uint32_t now_ms);
    void reset_run_locked(bool keep_status);
    void finish_check_locked(uint32_t team_id);
    void finish_sync_locked();
    void fail_locked(const char *error);
    void close_local_locked();
    void close_walk_locked();
    void release_walk_stack_locked();
    void clear_current_file_locked();
    bool ensure_walk_stack_locked();
    bool push_dir_locked(const char *path);
    bool ensure_dir_open_locked(WalkFrame &frame);
    bool root_step_locked();
    bool walk_step_locked();
    bool next_file_locked();
    bool plan_file_locked(const char *path);
    bool build_endpoint_state_dir_locked(uint32_t team_id,
                                         char *out,
                                         size_t out_size) const;
    bool ensure_state_dir_locked();
    bool build_state_path_locked(const char *path,
                                 char *out,
                                 size_t out_size,
                                 StateWriteMode *mode = nullptr) const;
    bool state_contains_locked(const char *state_path,
                               const char *path,
                               uint64_t size,
                               uint64_t mtime);
    void note_state_written_locked(const char *state_path,
                                   const char *path,
                                   uint64_t size,
                                   uint64_t mtime,
                                   StateWriteMode mode);
    bool write_state_locked(const StagedFile &file);
    bool append_state_locked(const StagedFile &file);
    bool replace_state_locked(const StagedFile &file);
    bool reserve_staged_locked(size_t needed);
    bool add_staged_locked(const SleepHqUploadResult &upload);
    void clear_staged_locked();
    bool local_ensure_dir_locked(const char *path);
    bool build_sleep_path_locked(const char *local_path,
                                 char *path_out,
                                 size_t path_out_size,
                                 char *name_out,
                                 size_t name_out_size) const;

    JobStep step_connect_locked(char *error, size_t error_size);
    JobStep step_check_locked(char *error, size_t error_size);
    JobStep step_create_import_locked(char *error, size_t error_size);
    JobStep step_open_local_locked();
    JobStep step_upload_file_locked(char *error, size_t error_size);
    JobStep step_process_import_locked(char *error, size_t error_size);
    JobStep step_wait_import_locked(char *error, size_t error_size);
    JobStep step_mark_state_locked(char *error, size_t error_size);
    JobStep step_work_phase_locked();

    static bool upload_read_cb(void *ctx, uint8_t *out,
                               size_t len, size_t &read);
    static bool upload_reset_cb(void *ctx);
    static bool upload_abort_cb(void *ctx);

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
    WalkFrame *walk_stack_ = nullptr;
    size_t walk_depth_ = 0;
    size_t walk_capacity_ = 0;
    size_t root_index_ = 0;
    CurrentFile current_file_;
    StagedFile *staged_ = nullptr;
    size_t staged_count_ = 0;
    size_t staged_capacity_ = 0;
    size_t mark_index_ = 0;
    uint32_t import_process_started_ms_ = 0;
    uint32_t import_poll_due_ms_ = 0;
    StorageExportStateCache state_cache_;
    char state_dir_[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
};

}  // namespace aircannect
