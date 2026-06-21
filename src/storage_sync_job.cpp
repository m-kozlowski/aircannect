#include "storage_sync_job.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crc32.h"
#include "debug_log.h"
#include "edf_storage_worker.h"
#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_export_plan.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t CONFIG_REFRESH_INTERVAL_MS = 1000;
static constexpr const char *SYNC_METADATA_FILE = "meta.state";
static constexpr const char *SYNC_REASON_STARTUP_CHECK = "startup_check";
static constexpr const char *SYNC_REASON_STARTUP_SYNC = "startup_sync";
static constexpr const char *SYNC_REASON_VERIFY_RECENT = "verify_recent";
static constexpr uint64_t SYNC_RECONCILE_INTERVAL_SECONDS =
    24ULL * 60ULL * 60ULL;
const uint32_t SYNC_RETRY_BACKOFF_MS[] = {
    15UL * 60UL * 1000UL,
    60UL * 60UL * 1000UL,
    6UL * 60UL * 60UL * 1000UL,
    6UL * 60UL * 60UL * 1000UL,
};

uint32_t millis_nonzero() {
    uint32_t now = millis();
    return now == 0 ? 1 : now;
}

bool parse_uint64_text(const char *text, uint64_t &out) {
    if (!text || !*text) return false;
    char *end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);
    if (!end || *end != '\0') return false;
    out = static_cast<uint64_t>(value);
    return true;
}

bool parse_uint32_text(const char *text, uint32_t &out) {
    uint64_t value = 0;
    if (!parse_uint64_text(text, value) || value > UINT32_MAX) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

}  // namespace

const char *storage_sync_state_name(StorageSyncState state) {
    switch (state) {
        case StorageSyncState::Disabled: return "disabled";
        case StorageSyncState::Idle: return "idle";
        case StorageSyncState::Pending: return "pending";
        case StorageSyncState::Working: return "working";
        case StorageSyncState::Error: return "error";
    }
    return "unknown";
}

void StorageSyncJob::begin(const AppConfigData &config) {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    configure(config);
}

bool StorageSyncJob::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageSyncJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StorageSyncJob::copy_string(char *dst, size_t dst_size,
                                 const String &src) {
    if (!dst || dst_size == 0) return;
    const size_t len = src.length();
    const size_t copy_len = len < dst_size - 1 ? len : dst_size - 1;
    if (copy_len) memcpy(dst, src.c_str(), copy_len);
    dst[copy_len] = '\0';
}

StorageSyncJob::ConfigSnapshot StorageSyncJob::make_config_snapshot(
    const AppConfigData &config) {
    ConfigSnapshot out;
    copy_string(out.endpoint, sizeof(out.endpoint), config.smb_endpoint);
    copy_string(out.user, sizeof(out.user), config.smb_user);
    copy_string(out.password, sizeof(out.password), config.smb_password);
    out.enabled = out.endpoint[0] != '\0';
    return out;
}

bool StorageSyncJob::snapshot_configured(const ConfigSnapshot &config) {
    return config.enabled && config.endpoint[0] != '\0';
}

const char *StorageSyncJob::work_phase_name(WorkPhase phase) {
    switch (phase) {
        case WorkPhase::Idle: return "idle";
        case WorkPhase::Connect: return "connect";
        case WorkPhase::VerifyLatestStart: return "verify_latest_start";
        case WorkPhase::VerifyLatestFile: return "verify_latest_file";
        case WorkPhase::VerifyLatestInvalidate:
            return "verify_latest_invalidate";
        case WorkPhase::NextFile: return "next_file";
        case WorkPhase::EnsureRemoteDir: return "ensure_remote_dir";
        case WorkPhase::OpenLocal: return "open_local";
        case WorkPhase::OpenRemote: return "open_remote";
        case WorkPhase::UploadChunk: return "upload_chunk";
        case WorkPhase::CloseRemote: return "close_remote";
        case WorkPhase::MarkState: return "mark_state";
        case WorkPhase::Finish: return "finish";
    }
    return "unknown";
}

const char *StorageSyncJob::run_kind_reason(RunKind kind) {
    switch (kind) {
        case RunKind::Manual: return "manual";
        case RunKind::PostTherapy: return "post_therapy";
        case RunKind::StartupCheck: return SYNC_REASON_STARTUP_CHECK;
        case RunKind::StartupSync: return SYNC_REASON_STARTUP_SYNC;
        case RunKind::VerifyRecent: return SYNC_REASON_VERIFY_RECENT;
        case RunKind::Retry: return "retry";
    }
    return "manual";
}

bool StorageSyncJob::run_kind_is_verify(RunKind kind) {
    return kind == RunKind::StartupCheck ||
           kind == RunKind::VerifyRecent;
}

bool StorageSyncJob::run_kind_is_reconcile(RunKind kind) {
    return kind == RunKind::VerifyRecent;
}

bool StorageSyncJob::config_matches_locked(
    const ConfigSnapshot &config) const {
    return config_.enabled == config.enabled &&
           strcmp(config_.endpoint, config.endpoint) == 0 &&
           strcmp(config_.user, config.user) == 0 &&
           strcmp(config_.password, config.password) == 0;
}

void StorageSyncJob::reset_run_locked(bool keep_status) {
    close_local_locked();
    close_latest_verify_locked();
    export_planner_.reset();
    release_upload_buffer_locked();
    smb_.abort_connection();
    clear_current_file_locked();
    phase_ = WorkPhase::Idle;
    endpoint_hash_ = 0;
    state_dir_[0] = '\0';
    current_run_kind_ = RunKind::Manual;
    sync_after_verify_ = false;
    ensured_remote_dir_[0] = '\0';
    latest_datalog_day_[0] = '\0';
    pending_run_kind_ = RunKind::Manual;
    state_cache_.clear();
    if (!keep_status) {
        const StorageSyncPersistentStatus preserved = status_;
        status_ = StorageSyncStatus();
        static_cast<StorageSyncPersistentStatus &>(status_) = preserved;
    }
}

bool StorageSyncJob::build_endpoint_state_dir_locked(
    const ConfigSnapshot &config,
    char *out,
    size_t out_size,
    uint32_t *hash_out) const {
    if (!out || out_size == 0 || !snapshot_configured(config)) return false;
    uint32_t crc = crc32_ieee_initial_state();
    storage_export_hash_update_cstr(crc, config.endpoint);
    storage_export_hash_update_cstr(crc, "\n");
    storage_export_hash_update_cstr(crc, config.user);
    const uint32_t hash = crc32_ieee_finish_state(crc);
    const int written = snprintf(out, out_size,
                                 "/aircannect/sync/smb/%08lx",
                                 static_cast<unsigned long>(hash));
    if (written <= 0 || static_cast<size_t>(written) >= out_size) {
        return false;
    }
    if (hash_out) *hash_out = hash;
    return true;
}

void StorageSyncJob::clear_result_metadata_locked() {
    status_.last_sync_epoch = 0;
    status_.last_sync_files_seen = 0;
    status_.last_sync_files_uploaded = 0;
    status_.last_sync_files_skipped = 0;
    status_.last_sync_files_failed = 0;
    status_.last_sync_bytes_uploaded = 0;
    status_.last_verify_epoch = 0;
    status_.last_verify_files_seen = 0;
    status_.last_reconcile_epoch = 0;
    status_.last_reconcile_files_seen = 0;
    status_.last_failure_epoch = 0;
    status_.last_failure_error[0] = '\0';
}

bool StorageSyncJob::load_result_metadata_locked() {
    clear_result_metadata_locked();
    if (!state_dir_[0]) return false;
    char path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    const int written = snprintf(path, sizeof(path), "%s/%s",
                                 state_dir_, SYNC_METADATA_FILE);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) {
        return false;
    }

    char buffer[512] = {};
    size_t read = 0;
    {
        Storage::Guard guard;
        File file = Storage::open(path, "r");
        if (!file) return false;
        read = file.read(reinterpret_cast<uint8_t *>(buffer),
                         sizeof(buffer) - 1);
        file.close();
    }
    buffer[read] = '\0';

    char *line = buffer;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }
        const size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *key = line;
            const char *value = eq + 1;
            if (strcmp(key, "last_sync_epoch") == 0) {
                (void)parse_uint64_text(value, status_.last_sync_epoch);
            } else if (strcmp(key, "last_sync_files_seen") == 0) {
                (void)parse_uint32_text(value,
                                        status_.last_sync_files_seen);
            } else if (strcmp(key, "last_sync_files_uploaded") == 0) {
                (void)parse_uint32_text(value,
                                        status_.last_sync_files_uploaded);
            } else if (strcmp(key, "last_sync_files_skipped") == 0) {
                (void)parse_uint32_text(value,
                                        status_.last_sync_files_skipped);
            } else if (strcmp(key, "last_sync_files_failed") == 0) {
                (void)parse_uint32_text(value,
                                        status_.last_sync_files_failed);
            } else if (strcmp(key, "last_sync_bytes_uploaded") == 0) {
                (void)parse_uint64_text(value,
                                        status_.last_sync_bytes_uploaded);
            } else if (strcmp(key, "last_verify_epoch") == 0) {
                (void)parse_uint64_text(value, status_.last_verify_epoch);
            } else if (strcmp(key, "last_verify_files_seen") == 0) {
                (void)parse_uint32_text(value,
                                        status_.last_verify_files_seen);
            } else if (strcmp(key, "last_reconcile_epoch") == 0) {
                (void)parse_uint64_text(value,
                                        status_.last_reconcile_epoch);
            } else if (strcmp(key, "last_reconcile_files_seen") == 0) {
                (void)parse_uint32_text(
                    value, status_.last_reconcile_files_seen);
            } else if (strcmp(key, "last_failure_epoch") == 0) {
                (void)parse_uint64_text(value, status_.last_failure_epoch);
            } else if (strcmp(key, "last_failure_error") == 0) {
                copy_cstr(status_.last_failure_error,
                          sizeof(status_.last_failure_error),
                          value);
            }
        }
        line = next;
    }
    return true;
}

bool StorageSyncJob::save_result_metadata_locked() {
    if (!state_dir_[0] || !ensure_state_dir_locked()) return false;
    char path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    const int path_written = snprintf(path, sizeof(path), "%s/%s",
                                      state_dir_, SYNC_METADATA_FILE);
    if (path_written <= 0 ||
        static_cast<size_t>(path_written) >= sizeof(path)) {
        return false;
    }
    {
        Storage::Guard guard;
        File file = Storage::open(path, "w");
        if (!file) return false;
        file.printf("version=1\n");
        file.printf("last_sync_epoch=%llu\n",
                    static_cast<unsigned long long>(
                        status_.last_sync_epoch));
        file.printf("last_sync_files_seen=%u\n",
                    static_cast<unsigned>(status_.last_sync_files_seen));
        file.printf("last_sync_files_uploaded=%u\n",
                    static_cast<unsigned>(status_.last_sync_files_uploaded));
        file.printf("last_sync_files_skipped=%u\n",
                    static_cast<unsigned>(status_.last_sync_files_skipped));
        file.printf("last_sync_files_failed=%u\n",
                    static_cast<unsigned>(status_.last_sync_files_failed));
        file.printf("last_sync_bytes_uploaded=%llu\n",
                    static_cast<unsigned long long>(
                        status_.last_sync_bytes_uploaded));
        file.printf("last_verify_epoch=%llu\n",
                    static_cast<unsigned long long>(
                        status_.last_verify_epoch));
        file.printf("last_verify_files_seen=%u\n",
                    static_cast<unsigned>(status_.last_verify_files_seen));
        file.printf("last_reconcile_epoch=%llu\n",
                    static_cast<unsigned long long>(
                        status_.last_reconcile_epoch));
        file.printf("last_reconcile_files_seen=%u\n",
                    static_cast<unsigned>(
                        status_.last_reconcile_files_seen));
        file.printf("last_failure_epoch=%llu\n",
                    static_cast<unsigned long long>(
                        status_.last_failure_epoch));
        file.printf("last_failure_error=%s\n", status_.last_failure_error);
        file.close();
    }
    return true;
}

void StorageSyncJob::apply_config_locked(const ConfigSnapshot &config) {
    const bool was_working = status_.state == StorageSyncState::Working;
    if (was_working) reset_run_locked(true);
    config_ = config;
    status_.enabled = config.enabled;
    status_.configured = snapshot_configured(config);
    status_.endpoint_set = config.endpoint[0] != '\0';
    status_.user_set = config.user[0] != '\0';
    status_.password_set = config.password[0] != '\0';
    status_.network_available = network_available_.load();
    status_.config_generation = next_config_generation_++;
    status_.updated_ms = millis_nonzero();
    endpoint_hash_ = 0;
    state_dir_[0] = '\0';
    clear_result_metadata_locked();
    if (status_.configured) {
        if (build_endpoint_state_dir_locked(config_,
                                            state_dir_,
                                            sizeof(state_dir_),
                                            &endpoint_hash_)) {
            (void)load_result_metadata_locked();
        } else {
            Log::logf(CAT_STORAGE,
                      LOG_WARN,
                      "[SYNC] state dir build failed endpoint=%s\n",
                      config_.endpoint);
        }
    }
    if (!status_.enabled) {
        status_.state = StorageSyncState::Disabled;
        status_.pending = false;
        status_.pending_reason[0] = '\0';
        status_.last_error[0] = '\0';
    } else if (!status_.configured) {
        status_.state = StorageSyncState::Error;
        copy_cstr(status_.last_error, sizeof(status_.last_error),
                  "missing_endpoint");
    } else if (status_.state == StorageSyncState::Disabled ||
               status_.state == StorageSyncState::Error) {
        status_.state = status_.pending ? StorageSyncState::Pending
                                        : StorageSyncState::Idle;
        status_.last_error[0] = '\0';
    }
    if (status_.enabled && status_.configured) {
        status_.state = StorageSyncState::Pending;
        status_.pending = true;
        pending_run_kind_ = RunKind::StartupCheck;
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  run_kind_reason(pending_run_kind_));
        status_.last_error[0] = '\0';
        status_.current_path[0] = '\0';
        status_.files_seen = 0;
        status_.files_uploaded = 0;
        status_.files_skipped = 0;
        status_.files_failed = 0;
        status_.bytes_uploaded = 0;
        status_.started_ms = 0;
        status_.last_run_verify = false;
    }
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    Log::logf(CAT_STORAGE,
              LOG_DEBUG,
              "[SYNC] config enabled=%u configured=%u\n",
              status_.enabled ? 1u : 0u,
              status_.configured ? 1u : 0u);
}

void StorageSyncJob::set_network_available(bool available) {
    const bool was_available = network_available_.exchange(available);
    if (was_available == available) return;
    if (!lock(5)) return;
    status_.network_available = available;
    unlock();
    if (available) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
}

void StorageSyncJob::defer_idle_work_until(uint32_t until_ms) {
    idle_defer_until_ms_.store(until_ms);
}

void StorageSyncJob::configure(const AppConfigData &config) {
    const ConfigSnapshot snapshot = make_config_snapshot(config);
    if (!lock(50)) return;
    if (!config_matches_locked(snapshot)) {
        apply_config_locked(snapshot);
    }
    unlock();
}

void StorageSyncJob::refresh_config(const AppConfigData &config,
                                    uint32_t now_ms) {
    if (last_config_check_ms_ != 0 &&
        static_cast<uint32_t>(now_ms - last_config_check_ms_) <
            CONFIG_REFRESH_INTERVAL_MS) {
        return;
    }
    last_config_check_ms_ = now_ms == 0 ? 1 : now_ms;
    configure(config);
}

bool StorageSyncJob::begin_run_locked() {
    const RunKind kind = pending_run_kind_;
    const char *run_reason = run_kind_reason(kind);
    reset_run_locked(false);
    current_run_kind_ = kind;
    retry_due_ms_ = 0;
    status_.state = StorageSyncState::Working;
    status_.pending = true;
    copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
              run_reason);
    status_.last_error[0] = '\0';
    status_.last_run_verify = run_kind_is_verify(current_run_kind_);
    status_.last_run_reconcile = run_kind_is_reconcile(current_run_kind_);
    status_.started_ms = millis_nonzero();
    status_.updated_ms = status_.started_ms;

    if (!build_endpoint_state_dir_locked(config_,
                                         state_dir_,
                                         sizeof(state_dir_),
                                         &endpoint_hash_)) {
        fail_locked("state_path_too_long");
        return false;
    }
    refresh_latest_datalog_day_name_locked();
    char planner_error[AC_STORAGE_ERROR_MAX] = {};
    if (!begin_export_planner_locked(planner_error, sizeof(planner_error))) {
        fail_locked(planner_error[0] ? planner_error : "planner_failed");
        return false;
    }
    phase_ = WorkPhase::Connect;
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] started reason=%s endpoint=%s\n",
              status_.pending_reason[0] ? status_.pending_reason : "manual",
              config_.endpoint);
    return true;
}

bool StorageSyncJob::request_sync_with_kind(RunKind kind, const char *label) {
    if (!lock(50)) {
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "[SYNC] %s request rejected reason=lock_timeout\n",
                  label ? label : "sync");
        return false;
    }
    if (!status_.enabled || !status_.configured) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] %s request rejected enabled=%u configured=%u\n",
                  label ? label : "sync",
                  status_.enabled ? 1u : 0u,
                  status_.configured ? 1u : 0u);
        unlock();
        return false;
    }
    if (status_.state != StorageSyncState::Working) {
        status_.pending = true;
        status_.state = StorageSyncState::Pending;
        pending_run_kind_ = kind;
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  run_kind_reason(kind));
        status_.updated_ms = millis_nonzero();
        retry_due_ms_ = 0;
        retry_attempt_ = 0;
        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SYNC] queued reason=%s state=%s\n",
                  status_.pending_reason,
                  storage_sync_state_name(status_.state));
    }
    unlock();
    if (BackgroundWorker *worker = background_worker()) worker->wake();
    return true;
}

bool StorageSyncJob::request_manual_sync() {
    return request_sync_with_kind(RunKind::Manual, "manual");
}

bool StorageSyncJob::request_verify_recent() {
    return request_sync_with_kind(RunKind::VerifyRecent, "verify_recent");
}

bool StorageSyncJob::request_post_therapy_sync() {
    if (!lock(50)) {
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "[SYNC] post-therapy request rejected reason=lock_timeout\n");
        return false;
    }
    if (!status_.enabled || !status_.configured) {
        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SYNC] post-therapy request ignored enabled=%u "
                  "configured=%u\n",
                  status_.enabled ? 1u : 0u,
                  status_.configured ? 1u : 0u);
        unlock();
        return false;
    }
    if (status_.state != StorageSyncState::Working) {
        status_.pending = true;
        status_.state = StorageSyncState::Pending;
        pending_run_kind_ = RunKind::PostTherapy;
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  run_kind_reason(pending_run_kind_));
        status_.updated_ms = millis_nonzero();
        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SYNC] queued reason=post_therapy\n");
    }
    unlock();
    if (BackgroundWorker *worker = background_worker()) worker->wake();
    return true;
}

bool StorageSyncJob::prepare_upload_buffer_locked() {
    if (upload_buffer_) return true;
    upload_buffer_size_ = AC_STORAGE_SYNC_UPLOAD_CHUNK;
    upload_buffer_ = static_cast<uint8_t *>(
        Memory::alloc_large(upload_buffer_size_, true));
    if (!upload_buffer_) {
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[SYNC] upload buffer allocation failed bytes=%u\n",
                  static_cast<unsigned>(upload_buffer_size_));
        return false;
    }
    return true;
}

void StorageSyncJob::release_upload_buffer_locked() {
    if (upload_buffer_) {
        Memory::free(upload_buffer_);
        upload_buffer_ = nullptr;
    }
    upload_buffer_size_ = 0;
}

bool StorageSyncJob::latest_datalog_day_locked(char *out,
                                               size_t out_size,
                                               char *error_out,
                                               size_t error_out_size) const {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    File dir;
    StorageLocalNodeInfo dir_info;
    {
        Storage::Guard guard;
        dir = Storage::open("/DATALOG", "r");
        storage_read_local_node_info(dir, dir_info);
    }
    if (!dir_info.exists) return true;
    if (!dir_info.is_dir) {
        Storage::Guard guard;
        dir.close();
        return true;
    }

    char best[9] = {};
    for (;;) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(dir, child)) break;
        if (child.is_dir && storage_export_is_datalog_day_name(child.name) &&
            (best[0] == '\0' || strcmp(child.name, best) > 0)) {
            copy_cstr(best, sizeof(best), child.name);
        }
    }
    {
        Storage::Guard guard;
        dir.close();
    }
    if (!best[0]) return true;
    const int written = snprintf(out, out_size, "/DATALOG/%s", best);
    if (written <= 0 || static_cast<size_t>(written) >= out_size) {
        copy_cstr(error_out, error_out_size, "latest_day_path_too_long");
        return false;
    }
    return true;
}

void StorageSyncJob::close_latest_verify_locked() {
    if (latest_verify_.opened) {
        Storage::Guard guard;
        latest_verify_.dir.close();
        latest_verify_.opened = false;
    }
    latest_verify_ = LatestVerify();
}

bool StorageSyncJob::begin_latest_verify_locked(char *error_out,
                                                size_t error_out_size) {
    close_latest_verify_locked();
    if (!latest_datalog_day_locked(latest_verify_.day_path,
                                   sizeof(latest_verify_.day_path),
                                   error_out,
                                   error_out_size)) {
        return false;
    }
    if (!latest_verify_.day_path[0]) {
        phase_ = WorkPhase::Finish;
        return true;
    }
    if (!build_state_path_locked(latest_verify_.day_path,
                                 latest_verify_.state_path,
                                 sizeof(latest_verify_.state_path))) {
        copy_cstr(error_out, error_out_size, "state_path_failed");
        return false;
    }
    {
        Storage::Guard guard;
        latest_verify_.dir = Storage::open(latest_verify_.day_path, "r");
        if (!latest_verify_.dir) {
            phase_ = WorkPhase::Finish;
            return true;
        }
        if (!latest_verify_.dir.isDirectory()) {
            latest_verify_.dir.close();
            phase_ = WorkPhase::Finish;
            return true;
        }
    }
    latest_verify_.opened = true;
    phase_ = WorkPhase::VerifyLatestFile;
    return true;
}

bool StorageSyncJob::latest_verify_file_step_locked(char *error_out,
                                                    size_t error_out_size) {
    if (!latest_verify_.opened) {
        phase_ = WorkPhase::Finish;
        return true;
    }

    StorageDirChild child;
    if (!storage_read_next_dir_child(latest_verify_.dir, child)) {
        {
            Storage::Guard guard;
            latest_verify_.dir.close();
            latest_verify_.opened = false;
        }
        phase_ = latest_verify_.invalidate_state
            ? WorkPhase::VerifyLatestInvalidate
            : WorkPhase::Finish;
        return true;
    }
    if (child.is_dir) return true;

    char child_path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_append_child_path(latest_verify_.day_path,
                                   child.name,
                                   child_path,
                                   sizeof(child_path)) ||
        !storage_user_path_valid(child_path)) {
        copy_cstr(error_out, error_out_size, "bad_child_path");
        return false;
    }

    status_.files_seen++;
    status_.updated_ms = millis_nonzero();
    const uint64_t mtime = child.last_write > 0
        ? static_cast<uint64_t>(child.last_write)
        : 0;
    if (!state_contains_locked(latest_verify_.state_path,
                               child_path,
                               child.size,
                               mtime)) {
        sync_after_verify_ = true;
        return true;
    }

    char remote_path[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
    if (!smb_.make_remote_path(child_path,
                               remote_path,
                               sizeof(remote_path))) {
        copy_cstr(error_out, error_out_size, "remote_path_failed");
        return false;
    }
    StorageSmbRemoteStat remote;
    if (!smb_.stat(remote_path, remote, error_out, error_out_size)) {
        return false;
    }
    if (!remote.exists || remote.directory || remote.size != child.size) {
        sync_after_verify_ = true;
        latest_verify_.invalidate_state = true;
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] latest cache mismatch local=%s remote=%s "
                  "exists=%u dir=%u remote_size=%llu local_size=%llu\n",
                  child_path,
                  remote_path,
                  remote.exists ? 1u : 0u,
                  remote.directory ? 1u : 0u,
                  static_cast<unsigned long long>(remote.size),
                  static_cast<unsigned long long>(child.size));
        return true;
    }
    status_.files_skipped++;
    return true;
}

bool StorageSyncJob::invalidate_latest_state_locked(char *error_out,
                                                    size_t error_out_size) {
    bool removed = false;
    {
        Storage::Guard guard;
        removed = Storage::remove(latest_verify_.state_path);
    }
    if (!removed) {
        copy_cstr(error_out, error_out_size, "state_invalidate_failed");
        return false;
    }
    Log::logf(CAT_STORAGE,
              LOG_WARN,
              "[SYNC] invalidated latest state path=%s\n",
              latest_verify_.state_path);
    close_latest_verify_locked();
    phase_ = WorkPhase::Finish;
    return true;
}

bool StorageSyncJob::next_file_locked() {
    if (!Storage::mounted()) {
        fail_locked("storage_unavailable");
        return false;
    }

    StorageExportPlannerItem item;
    char error[AC_STORAGE_ERROR_MAX] = {};
    const StorageExportPlannerResult result =
        export_planner_.next(item, error, sizeof(error));
    switch (result) {
        case StorageExportPlannerResult::Item:
            if (item.kind ==
                StorageExportPlannerItemKind::DatalogDayComplete) {
                maybe_mark_completed_datalog_day_locked(item.datalog_day);
                return true;
            }
            return plan_file_locked(item);
        case StorageExportPlannerResult::Yield:
            return true;
        case StorageExportPlannerResult::Done:
            phase_ = WorkPhase::Finish;
            return true;
        case StorageExportPlannerResult::Error:
            fail_locked(error[0] ? error : "planner_failed");
            return false;
    }
    return true;
}

bool StorageSyncJob::begin_export_planner_locked(char *error_out,
                                                 size_t error_out_size) {
    StorageExportPlannerConfig config;
    config.scope = StorageExportPlannerScope::FullCard;
    config.state_dir = state_dir_;
    config.state_cache = &state_cache_;
    config.latest_datalog_day = latest_datalog_day_;
    config.skip_completed_finalized_datalog_days =
        !run_kind_is_reconcile(current_run_kind_);
    return export_planner_.begin(config, error_out, error_out_size);
}

bool StorageSyncJob::build_state_path_locked(const char *path,
                                             char *out,
                                             size_t out_size,
                                             StateWriteMode *write_mode) const {
    if (!path || !out || out_size == 0 || !state_dir_[0]) return false;
    StorageExportStateWriteMode export_mode =
        StorageExportStateWriteMode::Replace;
    const bool ok = storage_export_build_state_path(state_dir_,
                                                    path,
                                                    out,
                                                    out_size,
                                                    &export_mode);
    if (write_mode) {
        *write_mode =
            export_mode == StorageExportStateWriteMode::Append
                ? StateWriteMode::Append
                : StateWriteMode::Replace;
    }
    return ok;
}

bool StorageSyncJob::local_ensure_dir_locked(const char *path) {
    if (!path || path[0] != '/') return false;
    char current[AC_STORAGE_PATH_MAX] = {};
    size_t len = 0;
    current[len++] = '/';
    current[len] = '\0';
    const char *segment = path + 1;
    for (const char *p = segment;; ++p) {
        if (*p != '/' && *p != '\0') continue;
        const size_t seg_len = static_cast<size_t>(p - segment);
        if (seg_len > 0) {
            if (len > 1) current[len++] = '/';
            if (len + seg_len >= sizeof(current)) return false;
            memcpy(current + len, segment, seg_len);
            len += seg_len;
            current[len] = '\0';
            if (!Storage::ensure_dir(current)) return false;
        }
        if (*p == '\0') break;
        segment = p + 1;
    }
    return true;
}

bool StorageSyncJob::ensure_state_dir_locked() {
    return state_dir_[0] && local_ensure_dir_locked(state_dir_);
}

bool StorageSyncJob::state_contains_locked(const char *state_path,
                                           const char *path,
                                           uint64_t size,
                                           uint64_t mtime) {
    return state_cache_.contains(state_path, path, size, mtime);
}

bool StorageSyncJob::append_state_locked(const char *state_path,
                                         const char *path,
                                         uint64_t size,
                                         uint64_t mtime) {
    if (!ensure_state_dir_locked()) {
        fail_locked("state_dir_failed");
        return false;
    }
    File file;
    {
        Storage::Guard guard;
        file = Storage::open(state_path, "a");
        if (!file) return false;
        file.printf("%llu\t%llu\t%s\n",
                    static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(mtime),
                    path ? path : "");
        file.close();
    }
    return true;
}

bool StorageSyncJob::replace_state_locked(const char *state_path,
                                          const char *path,
                                          uint64_t size,
                                          uint64_t mtime) {
    if (!ensure_state_dir_locked()) {
        fail_locked("state_dir_failed");
        return false;
    }
    File file;
    {
        Storage::Guard guard;
        file = Storage::open(state_path, "w");
        if (!file) return false;
        const size_t written =
            file.printf("%llu\t%llu\t%s\n",
                        static_cast<unsigned long long>(size),
                        static_cast<unsigned long long>(mtime),
                        path ? path : "");
        file.close();
        if (written == 0) return false;
    }
    return true;
}

void StorageSyncJob::note_state_written_locked(const char *state_path,
                                               const char *path,
                                               uint64_t size,
                                               uint64_t mtime,
                                               StateWriteMode mode) {
    state_cache_.note_written(
        state_path,
        path,
        size,
        mtime,
        mode == StateWriteMode::Append
            ? StorageExportStateWriteMode::Append
            : StorageExportStateWriteMode::Replace);
}

bool StorageSyncJob::write_state_locked(const char *state_path,
                                        const char *path,
                                        uint64_t size,
                                        uint64_t mtime,
                                        StateWriteMode mode) {
    const bool ok = mode == StateWriteMode::Replace
        ? replace_state_locked(state_path, path, size, mtime)
        : append_state_locked(state_path, path, size, mtime);
    if (ok) note_state_written_locked(state_path, path, size, mtime, mode);
    return ok;
}

bool StorageSyncJob::remote_parent_dir_locked(const char *remote_path,
                                              char *out,
                                              size_t out_size) const {
    if (!remote_path || !out || out_size == 0) return false;
    const char *slash = strrchr(remote_path, '/');
    if (!slash) {
        out[0] = '\0';
        return true;
    }
    const size_t len = static_cast<size_t>(slash - remote_path);
    if (len >= out_size) return false;
    memcpy(out, remote_path, len);
    out[len] = '\0';
    return true;
}

bool StorageSyncJob::datalog_day_name_from_path(const char *path,
                                                char *out,
                                                size_t out_size) const {
    return storage_export_datalog_day_from_path(path, out, out_size);
}

bool StorageSyncJob::datalog_day_done_path_locked(const char *day,
                                                  char *out,
                                                  size_t out_size) const {
    return storage_export_build_done_path(state_dir_, day, out, out_size);
}

bool StorageSyncJob::datalog_day_done_locked(const char *day) const {
    char path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    if (!datalog_day_done_path_locked(day, path, sizeof(path))) return false;
    const StorageLocalNodeInfo info = storage_stat_local_node(path);
    return info.exists && !info.is_dir;
}

bool StorageSyncJob::mark_datalog_day_done_locked(const char *day) {
    char path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    if (!datalog_day_done_path_locked(day, path, sizeof(path))) return false;
    if (!ensure_state_dir_locked()) return false;
    {
        Storage::Guard guard;
        File file = Storage::open(path, "w");
        if (!file) return false;
        file.printf("done\n");
        file.close();
    }
    return true;
}

bool StorageSyncJob::datalog_day_is_finalized_locked(const char *day) const {
    if (!day || strlen(day) != 8 ||
        !storage_export_all_digits(day, 8) ||
        !latest_datalog_day_[0]) {
        return false;
    }
    return strcmp(day, latest_datalog_day_) < 0;
}

void StorageSyncJob::refresh_latest_datalog_day_name_locked() {
    latest_datalog_day_[0] = '\0';
    char path[AC_STORAGE_PATH_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
    if (!latest_datalog_day_locked(path, sizeof(path),
                                   error, sizeof(error))) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] latest DATALOG day scan failed error=%s\n",
                  error[0] ? error : "latest_day_failed");
        return;
    }
    (void)datalog_day_name_from_path(path,
                                     latest_datalog_day_,
                                     sizeof(latest_datalog_day_));
}

void StorageSyncJob::maybe_mark_completed_datalog_day_locked(
    const char *day) {
    if (run_kind_is_reconcile(current_run_kind_)) return;
    if (!day || !day[0]) return;
    if (!datalog_day_is_finalized_locked(day)) return;
    if (datalog_day_done_locked(day)) return;
    if (!mark_datalog_day_done_locked(day)) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] DATALOG done marker write failed day=%s\n",
                  day);
        return;
    }
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] DATALOG day complete day=%s\n",
              day);
}

bool StorageSyncJob::plan_file_locked(const StorageExportPlannerItem &item) {
    const char *path = item.path;
    clear_current_file_locked();
    copy_cstr(current_file_.path, sizeof(current_file_.path), path);
    copy_cstr(status_.current_path, sizeof(status_.current_path), path);
    status_.files_seen++;
    status_.updated_ms = millis_nonzero();

    if (!item.info.exists || item.info.is_dir) {
        phase_ = WorkPhase::NextFile;
        return true;
    }
    current_file_.size = item.info.size;
    current_file_.mtime = item.info.mtime;

    if (!item.state_path[0]) {
        fail_locked("state_path_failed");
        return false;
    }
    copy_cstr(current_file_.state_path,
              sizeof(current_file_.state_path),
              item.state_path);
    current_file_.state_write_mode =
        item.state_write_mode == StorageExportStateWriteMode::Append
            ? StateWriteMode::Append
            : StateWriteMode::Replace;
    const bool local_state_complete = item.local_state_complete;
    if (local_state_complete) {
        if (!run_kind_is_reconcile(current_run_kind_)) {
            status_.files_skipped++;
            clear_current_file_locked();
            phase_ = WorkPhase::NextFile;
            return true;
        }
    }
    if (!smb_.make_remote_path(path,
                               current_file_.remote_path,
                               sizeof(current_file_.remote_path)) ||
        !remote_parent_dir_locked(current_file_.remote_path,
                                  current_file_.remote_dir,
                                  sizeof(current_file_.remote_dir))) {
        fail_locked("remote_path_failed");
        return false;
    }
    if (run_kind_is_reconcile(current_run_kind_)) {
        char error[AC_STORAGE_ERROR_MAX] = {};
        StorageSmbRemoteStat remote;
        if (!smb_.stat(current_file_.remote_path,
                       remote,
                       error,
                       sizeof(error))) {
            fail_locked(error[0] ? error : "remote_stat_failed");
            return false;
        }
        if (remote.exists && !remote.directory &&
            remote.size == item.info.size) {
            if (!local_state_complete) {
                if (!write_state_locked(current_file_.state_path,
                                        current_file_.path,
                                        current_file_.size,
                                        current_file_.mtime,
                                        current_file_.state_write_mode)) {
                    if (status_.state != StorageSyncState::Error) {
                        fail_locked("state_write_failed");
                    }
                    return false;
                }
            }
            status_.files_skipped++;
            clear_current_file_locked();
            phase_ = WorkPhase::NextFile;
            return true;
        }
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] reconcile upload local=%s remote=%s exists=%u "
                  "dir=%u remote_size=%llu local_size=%llu\n",
                  path,
                  current_file_.remote_path,
                  remote.exists ? 1u : 0u,
                  remote.directory ? 1u : 0u,
                  static_cast<unsigned long long>(remote.size),
                  static_cast<unsigned long long>(item.info.size));
    }
    phase_ = WorkPhase::EnsureRemoteDir;
    return true;
}

void StorageSyncJob::close_local_locked() {
    if (current_file_.local_open) {
        Storage::Guard guard;
        current_file_.local.close();
        current_file_.local_open = false;
    }
}

void StorageSyncJob::clear_current_file_locked() {
    close_local_locked();
    current_file_ = CurrentFile();
    status_.current_path[0] = '\0';
}

void StorageSyncJob::finish_run_locked() {
    smb_.disconnect();
    close_latest_verify_locked();
    export_planner_.reset();
    release_upload_buffer_locked();
    state_cache_.clear();
    clear_current_file_locked();
    ensured_remote_dir_[0] = '\0';
    const bool current_run_verify = run_kind_is_verify(current_run_kind_);
    const bool current_run_reconcile =
        run_kind_is_reconcile(current_run_kind_);
    const bool queue_sync =
        current_run_verify && !current_run_reconcile && sync_after_verify_;
    status_.state = queue_sync ? StorageSyncState::Pending
                               : StorageSyncState::Idle;
    status_.pending = queue_sync;
    if (queue_sync) {
        pending_run_kind_ = RunKind::StartupSync;
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  run_kind_reason(pending_run_kind_));
    } else {
        pending_run_kind_ = RunKind::Manual;
        status_.pending_reason[0] = '\0';
    }
    status_.last_error[0] = '\0';
    status_.updated_ms = millis_nonzero();
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    phase_ = WorkPhase::Idle;
    if (current_run_verify) {
        status_.last_verify_epoch = storage_export_current_epoch_seconds_or_zero();
        status_.last_verify_files_seen = status_.files_seen;
        if (current_run_reconcile) {
            status_.last_reconcile_epoch = status_.last_verify_epoch;
            status_.last_reconcile_files_seen = status_.files_seen;
        }
        if (current_run_reconcile && status_.files_uploaded > 0) {
            status_.last_sync_epoch = status_.last_verify_epoch;
            status_.last_sync_files_seen = status_.files_seen;
            status_.last_sync_files_uploaded = status_.files_uploaded;
            status_.last_sync_files_skipped = status_.files_skipped;
            status_.last_sync_files_failed = status_.files_failed;
            status_.last_sync_bytes_uploaded = status_.bytes_uploaded;
        }
        Log::logf(CAT_STORAGE,
                  queue_sync ? LOG_WARN : LOG_INFO,
                  "[SYNC] verify ok endpoint=%s files=%u remote_ok=%u "
                  "uploaded=%u followup=%s\n",
                  config_.endpoint,
                  static_cast<unsigned>(status_.files_seen),
                  static_cast<unsigned>(status_.files_skipped),
                  static_cast<unsigned>(status_.files_uploaded),
                  queue_sync ? "sync" : "none");
    } else {
        status_.last_sync_epoch = storage_export_current_epoch_seconds_or_zero();
        status_.last_sync_files_seen = status_.files_seen;
        status_.last_sync_files_uploaded = status_.files_uploaded;
        status_.last_sync_files_skipped = status_.files_skipped;
        status_.last_sync_files_failed = status_.files_failed;
        status_.last_sync_bytes_uploaded = status_.bytes_uploaded;
        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SYNC] done seen=%u uploaded=%u skipped=%u failed=%u "
                  "bytes=%llu\n",
                  static_cast<unsigned>(status_.files_seen),
                  static_cast<unsigned>(status_.files_uploaded),
                  static_cast<unsigned>(status_.files_skipped),
                  static_cast<unsigned>(status_.files_failed),
                  static_cast<unsigned long long>(status_.bytes_uploaded));
    }
    if (!save_result_metadata_locked()) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata save failed path=%s/%s\n",
                  state_dir_[0] ? state_dir_ : "--",
                  SYNC_METADATA_FILE);
    }
    current_run_kind_ = RunKind::Manual;
    sync_after_verify_ = false;
}

void StorageSyncJob::fail_locked(const char *error) {
    const WorkPhase failed_phase = phase_;
    const RunKind failed_kind = current_run_kind_;
    const bool current_run_verify = run_kind_is_verify(current_run_kind_);
    const bool current_run_reconcile =
        run_kind_is_reconcile(current_run_kind_);
    const bool failed_verify_only =
        current_run_verify && !current_run_reconcile;
    close_local_locked();
    export_planner_.reset();
    close_latest_verify_locked();
    smb_.abort_connection();
    release_upload_buffer_locked();
    state_cache_.clear();
    ensured_remote_dir_[0] = '\0';
    status_.state = StorageSyncState::Error;
    status_.pending = false;
    if (!failed_verify_only) status_.files_failed++;
    copy_cstr(status_.last_error, sizeof(status_.last_error),
              error ? error : "sync_error");
    status_.last_failure_epoch = storage_export_current_epoch_seconds_or_zero();
    copy_cstr(status_.last_failure_error,
              sizeof(status_.last_failure_error),
              status_.last_error);
    status_.updated_ms = millis_nonzero();
    phase_ = WorkPhase::Idle;
    if (status_.enabled && status_.configured) {
        const size_t backoff_count =
            sizeof(SYNC_RETRY_BACKOFF_MS) / sizeof(SYNC_RETRY_BACKOFF_MS[0]);
        const size_t index = retry_attempt_ < backoff_count ?
            retry_attempt_ : backoff_count - 1;
        retry_due_ms_ = status_.updated_ms + SYNC_RETRY_BACKOFF_MS[index];
        if (retry_attempt_ < 255) retry_attempt_++;
    } else {
        retry_due_ms_ = 0;
        retry_attempt_ = 0;
    }
    const uint32_t retry_in_ms =
        retry_due_ms_ ? static_cast<uint32_t>(retry_due_ms_ -
                                              status_.updated_ms) : 0;
    Log::logf(CAT_STORAGE,
              LOG_WARN,
              "[SYNC] failed phase=%s error=%s local=%s remote=%s "
              "remote_dir=%s reason=%s retry_ms=%lu attempt=%u "
              "seen=%u uploaded=%u skipped=%u failed=%u bytes=%llu\n",
              work_phase_name(failed_phase),
              status_.last_error,
              current_file_.path[0] ? current_file_.path : "--",
              current_file_.remote_path[0] ? current_file_.remote_path : "--",
              current_file_.remote_dir[0] ? current_file_.remote_dir : "--",
              status_.pending_reason[0] ? status_.pending_reason : "--",
              static_cast<unsigned long>(retry_in_ms),
              static_cast<unsigned>(retry_attempt_),
              static_cast<unsigned>(status_.files_seen),
              static_cast<unsigned>(status_.files_uploaded),
              static_cast<unsigned>(status_.files_skipped),
              static_cast<unsigned>(status_.files_failed),
              static_cast<unsigned long long>(status_.bytes_uploaded));
    if (!save_result_metadata_locked()) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata save failed after error path=%s/%s\n",
                  state_dir_[0] ? state_dir_ : "--",
                  SYNC_METADATA_FILE);
    }
    pending_run_kind_ = failed_kind;
    current_run_kind_ = RunKind::Manual;
}

bool StorageSyncJob::verify_endpoint_base_locked(char *error_out,
                                                 size_t error_out_size) {
    char base_path[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
    if (!smb_.make_remote_path("/", base_path, sizeof(base_path))) {
        copy_cstr(error_out, error_out_size, "remote_path_failed");
        return false;
    }
    if (!smb_.ensure_directory(base_path, error_out, error_out_size)) {
        return false;
    }
    return true;
}

void StorageSyncJob::queue_retry_locked(uint32_t now_ms) {
    if (status_.state != StorageSyncState::Error || retry_due_ms_ == 0 ||
        static_cast<int32_t>(now_ms - retry_due_ms_) < 0) {
        return;
    }
    status_.pending = true;
    status_.state = StorageSyncState::Pending;
    if (!status_.pending_reason[0]) {
        if (pending_run_kind_ == RunKind::Manual &&
            status_.last_run_verify) {
            pending_run_kind_ = RunKind::StartupCheck;
        } else if (pending_run_kind_ == RunKind::Manual) {
            pending_run_kind_ = RunKind::Retry;
        }
        copy_cstr(status_.pending_reason,
                  sizeof(status_.pending_reason),
                  run_kind_reason(pending_run_kind_));
    }
    status_.updated_ms = now_ms;
}

void StorageSyncJob::queue_reconcile_if_due_locked(uint32_t now_ms) {
    if (status_.state != StorageSyncState::Idle ||
        !status_.enabled ||
        !status_.configured ||
        !network_available_.load()) {
        return;
    }
    const uint64_t now_epoch = storage_export_current_epoch_seconds_or_zero();
    const bool due =
        now_epoch != 0 &&
        (status_.last_reconcile_epoch == 0 ||
         now_epoch >= status_.last_reconcile_epoch +
             SYNC_RECONCILE_INTERVAL_SECONDS);
    if (!due) return;

    status_.pending = true;
    status_.state = StorageSyncState::Pending;
    pending_run_kind_ = RunKind::VerifyRecent;
    copy_cstr(status_.pending_reason,
              sizeof(status_.pending_reason),
              run_kind_reason(pending_run_kind_));
    status_.updated_ms = now_ms;
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] queued reason=%s last_reconcile=%llu\n",
              status_.pending_reason,
              static_cast<unsigned long long>(
                  status_.last_reconcile_epoch));
}

bool StorageSyncJob::prepare_step_locked(uint32_t now_ms, JobStep &result) {
    const uint32_t defer_until = idle_defer_until_ms_.load();
    if (defer_until != 0 &&
        static_cast<int32_t>(now_ms - defer_until) < 0 &&
        status_.state != StorageSyncState::Working) {
        result = status_.pending ? JobStep::Waiting : JobStep::Idle;
        return false;
    }
    queue_retry_locked(now_ms);
    queue_reconcile_if_due_locked(now_ms);

    const bool ready =
        status_.enabled && status_.configured &&
        (status_.pending || status_.state == StorageSyncState::Working);
    if (!ready) {
        result = JobStep::Idle;
        return false;
    }
    if (status_.pending && status_.state != StorageSyncState::Working &&
        !network_available_.load()) {
        status_.network_available = false;
        result = JobStep::Idle;
        return false;
    }
    status_.network_available = network_available_.load();
    const EdfStorageWorkerStatus edf = EdfStorageWorker::status();
    if (edf.busy || edf.queued > 0 || edf.open_file_count > 0) {
        result = JobStep::Waiting;
        return false;
    }
    if (status_.state != StorageSyncState::Working &&
        !begin_run_locked()) {
        result = JobStep::Idle;
        return false;
    }
    return true;
}

JobStep StorageSyncJob::step_connect_locked(char *error_out,
                                            size_t error_out_size) {
    if (!smb_.configure(config_.endpoint, config_.user,
                        config_.password, error_out, error_out_size) ||
        !smb_.connect(error_out, error_out_size)) {
        fail_locked(error_out[0] ? error_out : "smb_connect_failed");
        return JobStep::Idle;
    }
    if (run_kind_is_verify(current_run_kind_) &&
        !run_kind_is_reconcile(current_run_kind_)) {
        if (!verify_endpoint_base_locked(error_out, error_out_size)) {
            fail_locked(error_out[0] ? error_out : "endpoint_verify_failed");
            return JobStep::Idle;
        }
        phase_ = WorkPhase::VerifyLatestStart;
        return JobStep::Working;
    }
    if (!prepare_upload_buffer_locked()) {
        fail_locked("upload_buffer_alloc");
        return JobStep::Idle;
    }
    phase_ = WorkPhase::NextFile;
    return JobStep::Working;
}

JobStep StorageSyncJob::step_verify_latest_start_locked(
    char *error_out,
    size_t error_out_size) {
    if (!begin_latest_verify_locked(error_out, error_out_size)) {
        fail_locked(error_out[0] ? error_out : "latest_verify_start_failed");
        return JobStep::Idle;
    }
    return JobStep::Working;
}

JobStep StorageSyncJob::step_verify_latest_file_locked(
    char *error_out,
    size_t error_out_size) {
    if (!latest_verify_file_step_locked(error_out, error_out_size)) {
        fail_locked(error_out[0] ? error_out : "latest_verify_failed");
        return JobStep::Idle;
    }
    return JobStep::Working;
}

JobStep StorageSyncJob::step_verify_latest_invalidate_locked(
    char *error_out,
    size_t error_out_size) {
    if (!invalidate_latest_state_locked(error_out, error_out_size)) {
        fail_locked(error_out[0] ? error_out : "state_invalidate_failed");
        return JobStep::Idle;
    }
    return JobStep::Working;
}

JobStep StorageSyncJob::step_ensure_remote_dir_locked(
    char *error_out,
    size_t error_out_size) {
    if (strcmp(ensured_remote_dir_, current_file_.remote_dir) != 0) {
        if (!smb_.ensure_directory(current_file_.remote_dir,
                                   error_out,
                                   error_out_size)) {
            fail_locked(error_out[0] ? error_out : "remote_mkdir_failed");
            return JobStep::Idle;
        }
        copy_cstr(ensured_remote_dir_,
                  sizeof(ensured_remote_dir_),
                  current_file_.remote_dir);
    }
    phase_ = WorkPhase::OpenLocal;
    return JobStep::Working;
}

JobStep StorageSyncJob::step_open_local_locked() {
    Storage::Guard guard;
    current_file_.local = Storage::open(current_file_.path, "r");
    if (!current_file_.local || current_file_.local.isDirectory()) {
        fail_locked("local_open_failed");
        return JobStep::Idle;
    }
    current_file_.local_open = true;
    phase_ = WorkPhase::OpenRemote;
    return JobStep::Working;
}

JobStep StorageSyncJob::step_open_remote_locked(char *error_out,
                                                size_t error_out_size) {
    if (!smb_.open_writer(current_file_.remote_path,
                          error_out,
                          error_out_size)) {
        fail_locked(error_out[0] ? error_out : "remote_open_failed");
        return JobStep::Idle;
    }
    current_file_.offset = 0;
    phase_ = current_file_.size == 0 ? WorkPhase::CloseRemote
                                     : WorkPhase::UploadChunk;
    return JobStep::Working;
}

JobStep StorageSyncJob::step_upload_chunk_locked(char *error_out,
                                                 size_t error_out_size) {
    uint64_t remaining = current_file_.size - current_file_.offset;
    size_t to_read = upload_buffer_size_;
    if (remaining < to_read) to_read = static_cast<size_t>(remaining);
    size_t read = 0;
    {
        Storage::Guard guard;
        read = current_file_.local.read(upload_buffer_, to_read);
    }
    if (read != to_read) {
        fail_locked("local_read_short");
        return JobStep::Idle;
    }
    const int written =
        smb_.write(upload_buffer_, read, error_out, error_out_size);
    if (written < 0 || static_cast<size_t>(written) != read) {
        fail_locked(error_out[0] ? error_out : "remote_write_failed");
        return JobStep::Idle;
    }
    current_file_.offset += static_cast<uint64_t>(written);
    status_.bytes_uploaded += static_cast<uint64_t>(written);
    status_.updated_ms = millis_nonzero();
    if (current_file_.offset >= current_file_.size) {
        phase_ = WorkPhase::CloseRemote;
    }
    return JobStep::Working;
}

JobStep StorageSyncJob::step_close_remote_locked(char *error_out,
                                                 size_t error_out_size) {
    if (!smb_.close_writer(error_out, error_out_size)) {
        fail_locked(error_out[0] ? error_out : "remote_close_failed");
        return JobStep::Idle;
    }
    close_local_locked();
    phase_ = WorkPhase::MarkState;
    return JobStep::Working;
}

JobStep StorageSyncJob::step_mark_state_locked() {
    const StorageLocalNodeInfo info = storage_stat_local_node(current_file_.path);
    if (!info.exists || info.is_dir ||
        info.size != current_file_.size ||
        info.mtime != current_file_.mtime) {
        fail_locked("local_changed");
        return JobStep::Idle;
    }
    if (!write_state_locked(current_file_.state_path,
                            current_file_.path,
                            current_file_.size,
                            current_file_.mtime,
                            current_file_.state_write_mode)) {
        if (status_.state != StorageSyncState::Error) {
            fail_locked("state_write_failed");
        }
        return JobStep::Idle;
    }
    status_.files_uploaded++;
    clear_current_file_locked();
    phase_ = WorkPhase::NextFile;
    return JobStep::Working;
}

JobStep StorageSyncJob::step_work_phase_locked() {
    char error[AC_STORAGE_ERROR_MAX] = {};
    switch (phase_) {
        case WorkPhase::Idle:
            phase_ = WorkPhase::Connect;
            return JobStep::Working;

        case WorkPhase::Connect:
            return step_connect_locked(error, sizeof(error));

        case WorkPhase::VerifyLatestStart:
            return step_verify_latest_start_locked(error, sizeof(error));

        case WorkPhase::VerifyLatestFile:
            return step_verify_latest_file_locked(error, sizeof(error));

        case WorkPhase::VerifyLatestInvalidate:
            return step_verify_latest_invalidate_locked(error, sizeof(error));

        case WorkPhase::NextFile:
            return next_file_locked() ? JobStep::Working : JobStep::Idle;

        case WorkPhase::EnsureRemoteDir:
            return step_ensure_remote_dir_locked(error, sizeof(error));

        case WorkPhase::OpenLocal:
            return step_open_local_locked();

        case WorkPhase::OpenRemote:
            return step_open_remote_locked(error, sizeof(error));

        case WorkPhase::UploadChunk:
            return step_upload_chunk_locked(error, sizeof(error));

        case WorkPhase::CloseRemote:
            return step_close_remote_locked(error, sizeof(error));

        case WorkPhase::MarkState:
            return step_mark_state_locked();

        case WorkPhase::Finish:
            finish_run_locked();
            return JobStep::Idle;
    }
    return JobStep::Idle;
}

JobStep StorageSyncJob::step() {
    if (!lock(20)) return JobStep::Waiting;
    JobStep result = JobStep::Idle;
    if (prepare_step_locked(millis_nonzero(), result)) {
        result = step_work_phase_locked();
    }
    unlock();
    return result;
}

void StorageSyncJob::on_preempt() {
    if (!lock(5)) return;
    if (status_.state == StorageSyncState::Working) {
        const RunKind kind = current_run_kind_;
        reset_run_locked(true);
        status_.state = StorageSyncState::Pending;
        status_.pending = true;
        pending_run_kind_ = kind;
        status_.updated_ms = millis_nonzero();
        copy_cstr(status_.pending_reason,
                  sizeof(status_.pending_reason),
                  run_kind_reason(pending_run_kind_));
    }
    unlock();
}

StorageSyncStatus StorageSyncJob::status() const {
    StorageSyncStatus out;
    if (lock(20)) {
        out = status_;
        out.retry_due_ms = retry_due_ms_;
        out.retry_attempt = retry_attempt_;
        out.network_available = network_available_.load();
        unlock();
    } else {
        // Telemetry fallback only: preserve the last public fields instead of
        // fabricating an empty status while the worker holds the lock in SMB I/O.
        out = status_;
        out.state = StorageSyncState::Working;
        out.pending = true;
        out.network_available = network_available_.load();
        out.updated_ms = millis_nonzero();
    }
    return out;
}

bool StorageSyncJob::active() const {
    if (!lock(20)) return true;
    const bool out = status_.state == StorageSyncState::Working ||
                     ((status_.state == StorageSyncState::Pending ||
                       status_.pending) &&
                      network_available_.load());
    unlock();
    return out;
}

}  // namespace aircannect
