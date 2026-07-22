#include "storage_sync_job.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crc32.h"
#include "debug_log.h"
#include "storage_service.h"
#include "memory_manager.h"
#include "runtime_clock.h"
#include "storage_export_plan.h"
#include "storage_export_state.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t CONFIG_REFRESH_INTERVAL_MS = 1000;
static constexpr uint32_t SMB_OPERATION_TIMEOUT_MS = 20UL * 1000UL;
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

void StorageSyncJob::begin(const AppConfigData &config,
                           StorageScanPort &scan_port,
                           StorageReadPort &read_port) {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    inventory_loader_.begin(scan_port, read_port);
    configure(config);
}

bool StorageSyncJob::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageSyncJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StorageSyncJob::publish_runtime_locked() {
    runtime_state_.store(static_cast<uint8_t>(status_.state));
    runtime_pending_.store(status_.pending);
    runtime_enabled_.store(status_.enabled);
    runtime_configured_.store(status_.configured);
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
        case WorkPhase::LoadInventory: return "load_inventory";
        case WorkPhase::Connect: return "connect";
        case WorkPhase::VerifyLatestStart: return "verify_latest_start";
        case WorkPhase::VerifyLatestFile: return "verify_latest_file";
        case WorkPhase::VerifyLatestRemote: return "verify_latest_remote";
        case WorkPhase::VerifyLatestInvalidate:
            return "verify_latest_invalidate";
        case WorkPhase::NextFile: return "next_file";
        case WorkPhase::ResolveRemoteFile: return "resolve_remote_file";
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
    const ConfigSnapshot &active =
        pending_config_valid_ ? pending_config_ : config_;
    return active.enabled == config.enabled &&
           strcmp(active.endpoint, config.endpoint) == 0 &&
           strcmp(active.user, config.user) == 0 &&
           strcmp(active.password, config.password) == 0;
}

void StorageSyncJob::reset_run_locked(bool keep_status) {
    close_local_locked();
    close_latest_verify_locked();
    inventory_loader_.reset();
    export_inventory_.reset();
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
    pending_run_kind_ = RunKind::Manual;
    abort_requested_.store(false);
    inventory_requested_ = false;
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
    config_ = config;
    status_.enabled = config.enabled;
    status_.configured = snapshot_configured(config);
    status_.endpoint_set = config.endpoint[0] != '\0';
    status_.user_set = config.user[0] != '\0';
    status_.password_set = config.password[0] != '\0';
    status_.network_available = network_available_.load();
    status_.config_generation = next_config_generation_++;
    status_.updated_ms = nonzero_millis(millis());
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
    publish_runtime_locked();
}

void StorageSyncJob::apply_pending_config_locked() {
    if (!pending_config_valid_) return;

    const ConfigSnapshot pending = pending_config_;
    pending_config_valid_ = false;
    pending_config_ = ConfigSnapshot();

    if (status_.state == StorageSyncState::Working) {
        preempt_run_locked();
    }
    apply_config_locked(pending);
}

void StorageSyncJob::set_network_available(bool available) {
    const bool was_available = network_available_.exchange(available);
    if (was_available && !available) request_operation_abort();
    if (was_available == available) return;
    if (!lock(0)) return;
    status_.network_available = available;
    publish_runtime_locked();
    unlock();
    if (available) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
}

void StorageSyncJob::set_runtime_blocked(bool blocked) {
    const bool was_blocked = runtime_blocked_.exchange(blocked);
    if (!was_blocked && blocked) request_operation_abort();
    if (was_blocked && !blocked) {
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
        if (status_.state == StorageSyncState::Working) {
            pending_config_ = snapshot;
            pending_config_valid_ = true;
            request_operation_abort();
        } else {
            apply_config_locked(snapshot);
        }
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
    const ConfigSnapshot snapshot = make_config_snapshot(config);
    if (!lock(0)) return;
    last_config_check_ms_ = now_ms == 0 ? 1 : now_ms;
    if (!config_matches_locked(snapshot)) {
        if (status_.state == StorageSyncState::Working) {
            pending_config_ = snapshot;
            pending_config_valid_ = true;
            request_operation_abort();
        } else {
            apply_config_locked(snapshot);
        }
    }
    unlock();
}

bool StorageSyncJob::begin_run_locked() {
    const RunKind kind = pending_run_kind_;
    const char *run_reason = run_kind_reason(kind);
    reset_run_locked(false);
    abort_requested_.store(false);
    current_run_kind_ = kind;
    retry_due_ms_ = 0;
    status_.state = StorageSyncState::Working;
    status_.pending = true;
    copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
              run_reason);
    status_.last_error[0] = '\0';
    status_.last_run_verify = run_kind_is_verify(current_run_kind_);
    status_.last_run_reconcile = run_kind_is_reconcile(current_run_kind_);
    status_.started_ms = nonzero_millis(millis());
    status_.updated_ms = status_.started_ms;
    publish_runtime_locked();

    if (!build_endpoint_state_dir_locked(config_,
                                         state_dir_,
                                         sizeof(state_dir_),
                                         &endpoint_hash_)) {
        fail_locked("state_path_too_long");
        return false;
    }
    phase_ = WorkPhase::LoadInventory;
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] started reason=%s endpoint=%s\n",
              status_.pending_reason[0] ? status_.pending_reason : "manual",
              config_.endpoint);
    return true;
}

JobStep StorageSyncJob::step_load_inventory_locked() {
    if (!inventory_requested_) {
        const uint32_t generation = next_inventory_generation_;
        const OperationAdmission admission =
            inventory_loader_.request(state_dir_, generation);
        if (admission == OperationAdmission::Busy) return JobStep::Waiting;
        if (admission != OperationAdmission::Accepted) {
            fail_locked("export_inventory_rejected");
            return JobStep::Idle;
        }

        next_inventory_generation_++;
        if (next_inventory_generation_ == 0) next_inventory_generation_ = 1;
        inventory_requested_ = true;
    }

    char error[AC_STORAGE_ERROR_MAX] = {};
    const StorageExportInventoryLoadResult result =
        inventory_loader_.poll(error, sizeof(error));
    if (result == StorageExportInventoryLoadResult::Waiting) {
        return JobStep::Waiting;
    }
    if (result == StorageExportInventoryLoadResult::Error) {
        fail_locked(error[0] ? error : "export_inventory_failed");
        return JobStep::Idle;
    }

    export_inventory_ = inventory_loader_.snapshot();
    if (!export_inventory_) {
        fail_locked("export_inventory_missing");
        return JobStep::Idle;
    }

    char planner_error[AC_STORAGE_ERROR_MAX] = {};
    if (!begin_export_planner_locked(planner_error, sizeof(planner_error))) {
        fail_locked(planner_error[0] ? planner_error : "planner_failed");
        return JobStep::Idle;
    }

    phase_ = WorkPhase::Connect;
    return JobStep::Working;
}

bool StorageSyncJob::request_sync_with_kind(RunKind kind, const char *label) {
    if (!lock(0)) {
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
        status_.updated_ms = nonzero_millis(millis());
        retry_due_ms_ = 0;
        retry_attempt_ = 0;
        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SYNC] queued reason=%s state=%s\n",
                  status_.pending_reason,
                  storage_sync_state_name(status_.state));
    }
    publish_runtime_locked();
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

bool StorageSyncJob::queue_post_therapy_locked(uint32_t now_ms) {
    if (!status_.enabled || !status_.configured) return false;
    if (status_.state == StorageSyncState::Working) return false;

    status_.pending = true;
    status_.state = StorageSyncState::Pending;
    pending_run_kind_ = RunKind::PostTherapy;
    copy_cstr(status_.pending_reason,
              sizeof(status_.pending_reason),
              run_kind_reason(pending_run_kind_));
    status_.updated_ms = now_ms == 0 ? nonzero_millis(millis()) : now_ms;
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    publish_runtime_locked();
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] queued reason=post_therapy\n");
    return true;
}

void StorageSyncJob::queue_deferred_post_therapy_locked(uint32_t now_ms) {
    if (!post_therapy_requested_.load()) return;
    if (!status_.enabled || !status_.configured) {
        post_therapy_requested_.store(false);
        return;
    }
    if (status_.state == StorageSyncState::Working) return;
    post_therapy_requested_.store(false);
    (void)queue_post_therapy_locked(now_ms);
}

bool StorageSyncJob::request_post_therapy_sync() {
    if (!lock(0)) {
        post_therapy_requested_.store(true);
        if (BackgroundWorker *worker = background_worker()) worker->wake();
        return true;
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
    if (status_.state == StorageSyncState::Working) {
        post_therapy_requested_.store(true);
    } else {
        (void)queue_post_therapy_locked(nonzero_millis(millis()));
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

bool StorageSyncJob::operation_abort_cb(void *ctx) {
    StorageSyncJob *job = static_cast<StorageSyncJob *>(ctx);
    return !job || job->abort_requested_.load() ||
           job->runtime_blocked_.load();
}

BackgroundOperationControl StorageSyncJob::operation_control() const {
    BackgroundOperationControl operation;
    operation.started_ms = millis();
    operation.timeout_ms = SMB_OPERATION_TIMEOUT_MS;
    operation.should_abort = &StorageSyncJob::operation_abort_cb;
    operation.ctx = const_cast<StorageSyncJob *>(this);
    return operation;
}

void StorageSyncJob::request_operation_abort() {
    bool expected = false;
    if (abort_requested_.compare_exchange_strong(expected, true)) {
        operation_generation_.fetch_add(1);
    }
}

void StorageSyncJob::close_latest_verify_locked() {
    latest_verify_ = LatestVerify();
}

bool StorageSyncJob::begin_latest_verify_locked(char *error_out,
                                                size_t error_out_size) {
    close_latest_verify_locked();
    if (!export_inventory_) {
        copy_cstr(error_out, error_out_size, "export_inventory_missing");
        return false;
    }

    const char *latest_day = export_inventory_->latest_datalog_day();
    if (!latest_day || !latest_day[0]) {
        phase_ = WorkPhase::Finish;
        return true;
    }

    const int written = snprintf(latest_verify_.day_path,
                                 sizeof(latest_verify_.day_path),
                                 "/DATALOG/%s",
                                 latest_day);
    if (written <= 0 ||
        static_cast<size_t>(written) >= sizeof(latest_verify_.day_path)) {
        copy_cstr(error_out, error_out_size, "latest_day_path_too_long");
        return false;
    }
    if (!build_state_path_locked(latest_verify_.day_path,
                                 latest_verify_.state_path,
                                 sizeof(latest_verify_.state_path))) {
        copy_cstr(error_out, error_out_size, "state_path_failed");
        return false;
    }

    latest_verify_.active = true;
    phase_ = WorkPhase::VerifyLatestFile;
    return true;
}

bool StorageSyncJob::latest_verify_file_step_locked(char *error_out,
                                                    size_t error_out_size) {
    if (!latest_verify_.active || !export_inventory_) {
        phase_ = WorkPhase::Finish;
        return true;
    }

    size_t budget = 32;
    while (latest_verify_.source_index < export_inventory_->source_size() &&
           budget-- > 0) {
        StorageExportInventoryEntryView entry;
        const size_t index = latest_verify_.source_index++;
        char day[9] = {};
        if (!export_inventory_->entry(index, entry) ||
            !storage_export_datalog_day_from_descendant(entry.path,
                                                        day,
                                                        sizeof(day)) ||
            strcmp(day, export_inventory_->latest_datalog_day()) != 0) {
            continue;
        }

        status_.files_seen++;
        status_.updated_ms = nonzero_millis(millis());
        if (!entry.local_state_complete) {
            sync_after_verify_ = true;
            continue;
        }

        copy_cstr(latest_verify_.current_path,
                  sizeof(latest_verify_.current_path),
                  entry.path);
        latest_verify_.current_size = entry.info.size;
        if (!smb_.make_remote_path(entry.path,
                                   latest_verify_.remote_path,
                                   sizeof(latest_verify_.remote_path))) {
            copy_cstr(error_out, error_out_size, "remote_path_failed");
            return false;
        }

        phase_ = WorkPhase::VerifyLatestRemote;
        return true;
    }

    if (latest_verify_.source_index >= export_inventory_->source_size()) {
        latest_verify_.active = false;
        phase_ = latest_verify_.invalidate_state
            ? WorkPhase::VerifyLatestInvalidate
            : WorkPhase::Finish;
    }
    return true;
}

JobStep StorageSyncJob::publish_verify_latest_remote_locked(
    const StorageSmbRemoteStat &remote) {
    if (!remote.exists || remote.directory ||
        remote.size != latest_verify_.current_size) {
        sync_after_verify_ = true;
        latest_verify_.invalidate_state = true;
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] latest cache mismatch local=%s remote=%s "
                  "exists=%u dir=%u remote_size=%llu local_size=%llu\n",
                  latest_verify_.current_path,
                  latest_verify_.remote_path,
                  remote.exists ? 1u : 0u,
                  remote.directory ? 1u : 0u,
                  static_cast<unsigned long long>(remote.size),
                  static_cast<unsigned long long>(
                      latest_verify_.current_size));
    } else {
        status_.files_skipped++;
    }

    latest_verify_.current_path[0] = '\0';
    latest_verify_.remote_path[0] = '\0';
    latest_verify_.current_size = 0;
    phase_ = WorkPhase::VerifyLatestFile;
    return JobStep::Working;
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
        case StorageExportPlannerResult::DecisionRequired:
            fail_locked("unexpected_planner_decision");
            return false;
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
    if (!export_inventory_) {
        copy_cstr(error_out, error_out_size, "export_inventory_missing");
        return false;
    }

    StorageExportPlannerConfig config;
    config.scope = StorageExportPlannerScope::FullCard;
    config.state_dir = state_dir_;
    config.now_epoch = storage_export_current_epoch_seconds_or_zero();
    config.skip_completed_finalized_datalog_days =
        !run_kind_is_reconcile(current_run_kind_);
    return export_planner_.begin(config,
                                 export_inventory_,
                                 error_out,
                                 error_out_size);
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

bool StorageSyncJob::ensure_state_dir_locked() {
    return storage_export_ensure_state_dir(state_dir_);
}

bool StorageSyncJob::write_state_locked(const char *state_path,
                                        const char *path,
                                        uint64_t size,
                                        uint64_t mtime,
                                        StateWriteMode mode) {
    char line[AC_STORAGE_PATH_MAX + 64] = {};
    const int written = snprintf(line,
                                 sizeof(line),
                                 "%llu\t%llu\t%s",
                                 static_cast<unsigned long long>(size),
                                 static_cast<unsigned long long>(mtime),
                                 path ? path : "");
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(line)) {
        return false;
    }
    const StorageExportStateWriteMode export_mode =
        mode == StateWriteMode::Append
            ? StorageExportStateWriteMode::Append
            : StorageExportStateWriteMode::Replace;
    return storage_export_write_state_line(&state_cache_,
                                           state_dir_,
                                           state_path,
                                           path,
                                           size,
                                           mtime,
                                           export_mode,
                                           line,
                                           false);
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

void StorageSyncJob::maybe_mark_completed_datalog_day_locked(
    const char *day) {
    if (run_kind_is_reconcile(current_run_kind_)) return;
    if (!day || !day[0] || !export_inventory_) return;
    if (!storage_export_datalog_day_finalized(
            day,
            export_inventory_->latest_datalog_day())) {
        return;
    }
    if (export_inventory_->datalog_day_done(day)) return;
    if (!storage_export_mark_datalog_day_done(state_dir_, day)) {
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
    status_.updated_ms = nonzero_millis(millis());

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
    current_file_.local_state_complete = item.local_state_complete;
    if (current_file_.local_state_complete) {
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
        phase_ = WorkPhase::ResolveRemoteFile;
        return true;
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
    close_latest_verify_locked();
    export_planner_.reset();
    inventory_loader_.reset();
    export_inventory_.reset();
    inventory_requested_ = false;
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
    status_.updated_ms = nonzero_millis(millis());
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
        if (status_.last_sync_epoch != 0 &&
            status_.last_sync_epoch > status_.last_verify_epoch) {
            status_.last_verify_epoch = status_.last_sync_epoch;
            status_.last_verify_files_seen = status_.files_seen;
        }
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
    queue_deferred_post_therapy_locked(status_.updated_ms);
    publish_runtime_locked();
}

void StorageSyncJob::preempt_run_locked() {
    const RunKind kind = current_run_kind_;
    char reason[AC_STORAGE_SYNC_REASON_MAX] = {};
    copy_cstr(reason,
              sizeof(reason),
              status_.pending_reason[0]
                  ? status_.pending_reason
                  : run_kind_reason(kind));

    reset_run_locked(true);

    pending_run_kind_ = kind;
    status_.state = StorageSyncState::Pending;
    status_.pending = true;
    copy_cstr(status_.pending_reason, sizeof(status_.pending_reason), reason);
    status_.last_error[0] = '\0';
    status_.updated_ms = nonzero_millis(millis());
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    Log::logf(CAT_STORAGE,
              LOG_DEBUG,
              "[SYNC] preempted; queued reason=%s\n",
              status_.pending_reason);
    publish_runtime_locked();
}

void StorageSyncJob::fail_locked(const char *error) {
    if (error && strcmp(error, "preempted") == 0) {
        preempt_run_locked();
        return;
    }

    const WorkPhase failed_phase = phase_;
    const RunKind failed_kind = current_run_kind_;
    const bool current_run_verify = run_kind_is_verify(current_run_kind_);
    const bool current_run_reconcile =
        run_kind_is_reconcile(current_run_kind_);
    const bool failed_verify_only =
        current_run_verify && !current_run_reconcile;
    close_local_locked();
    export_planner_.reset();
    inventory_loader_.reset();
    export_inventory_.reset();
    inventory_requested_ = false;
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
    status_.updated_ms = nonzero_millis(millis());
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
    publish_runtime_locked();
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
    publish_runtime_locked();
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
    publish_runtime_locked();
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] queued reason=%s last_reconcile=%llu\n",
              status_.pending_reason,
              static_cast<unsigned long long>(
                  status_.last_reconcile_epoch));
}

bool StorageSyncJob::prepare_step_locked(uint32_t now_ms, JobStep &result) {
    apply_pending_config_locked();

    const uint32_t defer_until = idle_defer_until_ms_.load();
    if (defer_until != 0 &&
        static_cast<int32_t>(now_ms - defer_until) < 0 &&
        status_.state != StorageSyncState::Working) {
        result = status_.pending ? JobStep::Waiting : JobStep::Idle;
        return false;
    }
    queue_retry_locked(now_ms);
    queue_reconcile_if_due_locked(now_ms);
    queue_deferred_post_therapy_locked(now_ms);

    if (abort_requested_.load() &&
        status_.state == StorageSyncState::Working) {
        preempt_run_locked();
        result = status_.pending ? JobStep::Waiting : JobStep::Idle;
        return false;
    }

    if (runtime_blocked_.load()) {
        result = status_.pending ? JobStep::Waiting : JobStep::Idle;
        return false;
    }

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
    const StorageServiceStatus edf = StorageService::status();
    if (edf.busy || edf.edf_queued > 0 || edf.open_file_count > 0) {
        result = JobStep::Waiting;
        return false;
    }
    if (status_.state != StorageSyncState::Working &&
        edf.maintenance_active) {
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

bool StorageSyncJob::phase_has_blocking_io(WorkPhase phase) {
    switch (phase) {
        case WorkPhase::Connect:
        case WorkPhase::VerifyLatestRemote:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::EnsureRemoteDir:
        case WorkPhase::OpenRemote:
        case WorkPhase::UploadChunk:
        case WorkPhase::CloseRemote:
        case WorkPhase::Finish:
            return true;

        case WorkPhase::Idle:
        case WorkPhase::LoadInventory:
        case WorkPhase::VerifyLatestStart:
        case WorkPhase::VerifyLatestFile:
        case WorkPhase::VerifyLatestInvalidate:
        case WorkPhase::NextFile:
        case WorkPhase::OpenLocal:
        case WorkPhase::MarkState:
            return false;
    }
    return false;
}

void StorageSyncJob::execute_blocking_phase(WorkPhase phase,
                                            BlockingResult &result) {
    result = BlockingResult();
    BackgroundOperationControl operation = operation_control();

    switch (phase) {
        case WorkPhase::Connect: {
            if (!smb_.configure(config_.endpoint,
                                config_.user,
                                config_.password,
                                result.error,
                                sizeof(result.error),
                                &operation) ||
                !smb_.connect(result.error,
                              sizeof(result.error),
                              &operation)) {
                return;
            }

            if (run_kind_is_verify(current_run_kind_) &&
                !run_kind_is_reconcile(current_run_kind_)) {
                char base_path[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
                if (!smb_.make_remote_path("/",
                                           base_path,
                                           sizeof(base_path))) {
                    copy_cstr(result.error,
                              sizeof(result.error),
                              "remote_path_failed");
                    return;
                }
                if (!smb_.ensure_directory(base_path,
                                           result.error,
                                           sizeof(result.error),
                                           &operation)) {
                    return;
                }
            }
            result.ok = true;
            return;
        }

        case WorkPhase::VerifyLatestRemote:
            result.ok = smb_.stat(latest_verify_.remote_path,
                                  result.remote,
                                  result.error,
                                  sizeof(result.error),
                                  &operation);
            return;

        case WorkPhase::ResolveRemoteFile:
            result.ok = smb_.stat(current_file_.remote_path,
                                  result.remote,
                                  result.error,
                                  sizeof(result.error),
                                  &operation);
            return;

        case WorkPhase::EnsureRemoteDir:
            result.ok =
                strcmp(ensured_remote_dir_, current_file_.remote_dir) == 0 ||
                smb_.ensure_directory(current_file_.remote_dir,
                                      result.error,
                                      sizeof(result.error),
                                      &operation);
            return;

        case WorkPhase::OpenRemote:
            result.ok = smb_.open_writer(current_file_.remote_path,
                                         result.error,
                                         sizeof(result.error),
                                         &operation);
            return;

        case WorkPhase::UploadChunk: {
            const uint64_t remaining =
                current_file_.size - current_file_.offset;
            size_t to_read = upload_buffer_size_;
            if (remaining < to_read) {
                to_read = static_cast<size_t>(remaining);
            }

            size_t read = 0;
            {
                Storage::Guard guard;
                read = current_file_.local.read(upload_buffer_, to_read);
            }
            if (read != to_read) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          "local_read_short");
                return;
            }

            result.transferred = smb_.write(upload_buffer_,
                                            read,
                                            result.error,
                                            sizeof(result.error),
                                            &operation);
            result.ok = result.transferred >= 0 &&
                        static_cast<size_t>(result.transferred) == read;
            return;
        }

        case WorkPhase::CloseRemote:
            result.ok = smb_.close_writer(result.error,
                                          sizeof(result.error),
                                          &operation);
            return;

        case WorkPhase::Finish:
            smb_.disconnect(&operation);
            result.ok = true;
            return;

        case WorkPhase::Idle:
        case WorkPhase::LoadInventory:
        case WorkPhase::VerifyLatestStart:
        case WorkPhase::VerifyLatestFile:
        case WorkPhase::VerifyLatestInvalidate:
        case WorkPhase::NextFile:
        case WorkPhase::OpenLocal:
        case WorkPhase::MarkState:
            copy_cstr(result.error,
                      sizeof(result.error),
                      "invalid_blocking_phase");
            return;
    }
}

JobStep StorageSyncJob::publish_blocking_phase_locked(
    WorkPhase phase,
    const BlockingResult &result) {
    if (status_.state != StorageSyncState::Working || phase_ != phase) {
        smb_.abort_connection();
        return JobStep::Idle;
    }

    if (!background_operation_result_current(
            result.operation_generation,
            operation_generation_.load(),
            abort_requested_.load()) ||
        pending_config_valid_ ||
        runtime_blocked_.load() ||
        !network_available_.load()) {
        preempt_run_locked();
        apply_pending_config_locked();
        return status_.pending ? JobStep::Waiting : JobStep::Idle;
    }

    if (!result.ok) {
        fail_locked(result.error[0] ? result.error : "network_io_failed");
        return JobStep::Idle;
    }

    switch (phase) {
        case WorkPhase::Connect:
            if (run_kind_is_verify(current_run_kind_) &&
                !run_kind_is_reconcile(current_run_kind_)) {
                phase_ = WorkPhase::VerifyLatestStart;
                return JobStep::Working;
            }
            if (!prepare_upload_buffer_locked()) {
                fail_locked("upload_buffer_alloc");
                return JobStep::Idle;
            }
            phase_ = WorkPhase::NextFile;
            return JobStep::Working;

        case WorkPhase::VerifyLatestRemote:
            return publish_verify_latest_remote_locked(result.remote);

        case WorkPhase::ResolveRemoteFile:
            if (result.remote.exists && !result.remote.directory &&
                result.remote.size == current_file_.size) {
                if (!current_file_.local_state_complete &&
                    !write_state_locked(current_file_.state_path,
                                        current_file_.path,
                                        current_file_.size,
                                        current_file_.mtime,
                                        current_file_.state_write_mode)) {
                    fail_locked("state_write_failed");
                    return JobStep::Idle;
                }
                status_.files_skipped++;
                clear_current_file_locked();
                phase_ = WorkPhase::NextFile;
                return JobStep::Working;
            }

            Log::logf(CAT_STORAGE,
                      LOG_WARN,
                      "[SYNC] reconcile upload local=%s remote=%s "
                      "exists=%u dir=%u remote_size=%llu local_size=%llu\n",
                      current_file_.path,
                      current_file_.remote_path,
                      result.remote.exists ? 1u : 0u,
                      result.remote.directory ? 1u : 0u,
                      static_cast<unsigned long long>(result.remote.size),
                      static_cast<unsigned long long>(current_file_.size));
            phase_ = WorkPhase::EnsureRemoteDir;
            return JobStep::Working;

        case WorkPhase::EnsureRemoteDir:
            copy_cstr(ensured_remote_dir_,
                      sizeof(ensured_remote_dir_),
                      current_file_.remote_dir);
            phase_ = WorkPhase::OpenLocal;
            return JobStep::Working;

        case WorkPhase::OpenRemote:
            current_file_.offset = 0;
            phase_ = current_file_.size == 0
                ? WorkPhase::CloseRemote
                : WorkPhase::UploadChunk;
            return JobStep::Working;

        case WorkPhase::UploadChunk:
            current_file_.offset +=
                static_cast<uint64_t>(result.transferred);
            status_.bytes_uploaded +=
                static_cast<uint64_t>(result.transferred);
            status_.updated_ms = nonzero_millis(millis());
            if (current_file_.offset >= current_file_.size) {
                phase_ = WorkPhase::CloseRemote;
            }
            return JobStep::Working;

        case WorkPhase::CloseRemote:
            close_local_locked();
            phase_ = WorkPhase::MarkState;
            return JobStep::Working;

        case WorkPhase::Finish:
            finish_run_locked();
            return JobStep::Idle;

        case WorkPhase::Idle:
        case WorkPhase::LoadInventory:
        case WorkPhase::VerifyLatestStart:
        case WorkPhase::VerifyLatestFile:
        case WorkPhase::VerifyLatestInvalidate:
        case WorkPhase::NextFile:
        case WorkPhase::OpenLocal:
        case WorkPhase::MarkState:
            fail_locked("invalid_blocking_phase");
            return JobStep::Idle;
    }
    return JobStep::Idle;
}

JobStep StorageSyncJob::step_work_phase_locked() {
    char error[AC_STORAGE_ERROR_MAX] = {};
    switch (phase_) {
        case WorkPhase::Idle:
            phase_ = WorkPhase::LoadInventory;
            return JobStep::Working;

        case WorkPhase::LoadInventory:
            return step_load_inventory_locked();

        case WorkPhase::Connect:
        case WorkPhase::VerifyLatestRemote:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::EnsureRemoteDir:
        case WorkPhase::OpenRemote:
        case WorkPhase::UploadChunk:
        case WorkPhase::CloseRemote:
        case WorkPhase::Finish:
            return JobStep::Waiting;

        case WorkPhase::VerifyLatestStart:
            return step_verify_latest_start_locked(error, sizeof(error));

        case WorkPhase::VerifyLatestFile:
            return step_verify_latest_file_locked(error, sizeof(error));

        case WorkPhase::VerifyLatestInvalidate:
            return step_verify_latest_invalidate_locked(error, sizeof(error));

        case WorkPhase::NextFile:
            return next_file_locked() ? JobStep::Working : JobStep::Idle;

        case WorkPhase::OpenLocal:
            return step_open_local_locked();

        case WorkPhase::MarkState:
            return step_mark_state_locked();
    }
    return JobStep::Idle;
}

JobStep StorageSyncJob::step() {
    if (!lock(20)) return JobStep::Waiting;
    JobStep result = JobStep::Idle;
    if (!prepare_step_locked(nonzero_millis(millis()), result)) {
        unlock();
        return result;
    }

    const WorkPhase phase = phase_;
    if (!phase_has_blocking_io(phase)) {
        result = step_work_phase_locked();
        unlock();
        return result;
    }

    const uint32_t operation_generation = operation_generation_.load();
    unlock();

    BlockingResult blocking_result;
    execute_blocking_phase(phase, blocking_result);
    blocking_result.operation_generation = operation_generation;

    if (!lock_ || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) {
        smb_.abort_connection();
        Log::logf(CAT_STORAGE,
                  LOG_ERROR,
                  "[SYNC] state publish lock unavailable phase=%s\n",
                  work_phase_name(phase));
        return JobStep::Idle;
    }
    result = publish_blocking_phase_locked(phase, blocking_result);
    unlock();
    return result;
}

void StorageSyncJob::on_preempt() {
    request_operation_abort();
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
        const StorageSyncRuntimeStatus runtime = runtime_status();
        out.state = runtime.state;
        out.pending = runtime.pending;
        out.enabled = runtime.enabled;
        out.configured = runtime.configured;
        out.network_available = runtime.network_available;
    }
    if (out.last_sync_epoch != 0 &&
        out.last_sync_epoch > out.last_verify_epoch) {
        out.last_verify_epoch = out.last_sync_epoch;
        out.last_verify_files_seen = out.last_sync_files_seen;
    }
    return out;
}

StorageSyncRuntimeStatus StorageSyncJob::runtime_status() const {
    StorageSyncRuntimeStatus out;
    out.state = static_cast<StorageSyncState>(runtime_state_.load());
    out.pending = runtime_pending_.load();
    out.enabled = runtime_enabled_.load();
    out.configured = runtime_configured_.load();
    out.network_available = network_available_.load();
    return out;
}

bool StorageSyncJob::active() const {
    return runtime_status().active();
}

}  // namespace aircannect
