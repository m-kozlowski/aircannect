#include "storage_sync_engine.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "crc32.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "runtime_clock.h"
#include "storage_export_plan.h"
#include "storage_service.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t SMB_OPERATION_TIMEOUT_MS = 20UL * 1000UL;
static constexpr const char *SYNC_METADATA_FILE = "meta.state";
static constexpr size_t SYNC_METADATA_MAX_BYTES = 511;
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

std::shared_ptr<const LargeByteBuffer> freeze_bytes(const void *data,
                                                    size_t size) {
    if (!data || size == 0) return {};

    std::unique_ptr<LargeByteBuffer> bytes = LargeByteBuffer::allocate(size);
    if (!bytes) return {};

    memcpy(bytes->data(), data, size);
    return LargeByteBuffer::freeze(std::move(bytes));
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

void StorageSyncEngine::begin(const SmbExportConfig &config,
                           StorageScanPort &scan_port,
                           StorageReadPort &read_port,
                           StorageStreamPort &stream_port,
                           StorageAtomicWritePort &write_port,
                           StoragePathPort &path_port) {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    inventory_loader_.begin(scan_port, read_port);
    state_io_.begin(read_port, write_port, path_port);
    metadata_io_.begin(read_port, write_port, path_port);
    stream_port_ = &stream_port;
    configure(config);
}

bool StorageSyncEngine::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageSyncEngine::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void StorageSyncEngine::publish_runtime_locked() {
    runtime_state_.store(static_cast<uint8_t>(status_.state));
    runtime_pending_.store(status_.pending);
    runtime_enabled_.store(status_.enabled);
    runtime_configured_.store(status_.configured);
}

bool StorageSyncEngine::snapshot_configured(const SmbExportConfig &config) {
    return config.enabled && config.endpoint[0] != '\0';
}

const char *StorageSyncEngine::work_phase_name(WorkPhase phase) {
    switch (phase) {
        case WorkPhase::Idle: return "idle";
        case WorkPhase::LoadMetadata: return "load_metadata";
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
        case WorkPhase::ValidateLocal: return "validate_local";
        case WorkPhase::FlushState: return "flush_state";
        case WorkPhase::WriteDoneMarker: return "write_done_marker";
        case WorkPhase::Finish: return "finish";
    }
    return "unknown";
}

const char *StorageSyncEngine::run_kind_reason(RunKind kind) {
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

bool StorageSyncEngine::run_kind_is_verify(RunKind kind) {
    return kind == RunKind::StartupCheck ||
           kind == RunKind::VerifyRecent;
}

bool StorageSyncEngine::run_kind_is_reconcile(RunKind kind) {
    return kind == RunKind::VerifyRecent;
}

bool StorageSyncEngine::config_matches_locked(
    const SmbExportConfig &config) const {
    const SmbExportConfig &active =
        pending_config_valid_ ? pending_config_ : config_;
    return active.enabled == config.enabled &&
           strcmp(active.endpoint, config.endpoint) == 0 &&
           strcmp(active.user, config.user) == 0 &&
           strcmp(active.password, config.password) == 0;
}

void StorageSyncEngine::reset_run_locked(bool keep_status) {
    close_local_locked();
    close_latest_verify_locked();
    inventory_loader_.reset();
    export_inventory_.reset();
    export_planner_.reset();
    state_batch_.clear();
    state_io_.reset();
    metadata_io_.reset();
    metadata_file_.reset();
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
    pending_state_path_[0] = '\0';
    pending_done_day_[0] = '\0';
    pending_state_bytes_.reset();
    pending_state_uploaded_ = 0;
    pending_state_skipped_ = 0;
    if (!keep_status) {
        const StorageSyncPersistentStatus preserved = status_;
        status_ = StorageSyncStatus();
        static_cast<StorageSyncPersistentStatus &>(status_) = preserved;
    }
}

bool StorageSyncEngine::build_endpoint_state_dir_locked(
    const SmbExportConfig &config,
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

void StorageSyncEngine::clear_result_metadata_locked() {
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

void StorageSyncEngine::parse_result_metadata_locked(char *buffer) {
    if (!buffer) return;

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
}

bool StorageSyncEngine::queue_result_metadata_save_locked() {
    if (!state_dir_[0] || metadata_save_pending_) return false;

    const int path_written = snprintf(pending_metadata_path_,
                                      sizeof(pending_metadata_path_),
                                      "%s/%s",
                                      state_dir_,
                                      SYNC_METADATA_FILE);
    if (path_written <= 0 || static_cast<size_t>(path_written) >=
                                 sizeof(pending_metadata_path_)) {
        pending_metadata_path_[0] = '\0';
        return false;
    }

    char buffer[768] = {};
    const int written = snprintf(
        buffer,
        sizeof(buffer),
        "version=1\n"
        "last_sync_epoch=%llu\n"
        "last_sync_files_seen=%u\n"
        "last_sync_files_uploaded=%u\n"
        "last_sync_files_skipped=%u\n"
        "last_sync_files_failed=%u\n"
        "last_sync_bytes_uploaded=%llu\n"
        "last_verify_epoch=%llu\n"
        "last_verify_files_seen=%u\n"
        "last_reconcile_epoch=%llu\n"
        "last_reconcile_files_seen=%u\n"
        "last_failure_epoch=%llu\n"
        "last_failure_error=%s\n",
        static_cast<unsigned long long>(status_.last_sync_epoch),
        static_cast<unsigned>(status_.last_sync_files_seen),
        static_cast<unsigned>(status_.last_sync_files_uploaded),
        static_cast<unsigned>(status_.last_sync_files_skipped),
        static_cast<unsigned>(status_.last_sync_files_failed),
        static_cast<unsigned long long>(status_.last_sync_bytes_uploaded),
        static_cast<unsigned long long>(status_.last_verify_epoch),
        static_cast<unsigned>(status_.last_verify_files_seen),
        static_cast<unsigned long long>(status_.last_reconcile_epoch),
        static_cast<unsigned>(status_.last_reconcile_files_seen),
        static_cast<unsigned long long>(status_.last_failure_epoch),
        status_.last_failure_error);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
        pending_metadata_path_[0] = '\0';
        return false;
    }

    pending_metadata_bytes_ = freeze_bytes(buffer,
                                           static_cast<size_t>(written));
    if (!pending_metadata_bytes_) {
        pending_metadata_path_[0] = '\0';
        return false;
    }

    metadata_save_pending_ = true;
    return true;
}

bool StorageSyncEngine::service_result_metadata_save_locked(ExportStep &result) {
    if (!metadata_save_pending_) return true;

    const StorageFileClientResult io_result = metadata_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        result = ExportStep::Waiting;
        return false;
    }
    if (io_result == StorageFileClientResult::Ready) {
        metadata_io_.reset();
        pending_metadata_path_[0] = '\0';
        pending_metadata_bytes_.reset();
        metadata_save_pending_ = false;
        return true;
    }
    if (io_result == StorageFileClientResult::Error) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata save failed path=%s error=%s\n",
                  pending_metadata_path_[0] ? pending_metadata_path_ : "--",
                  metadata_io_.error()[0] ? metadata_io_.error()
                                          : "storage_write_failed");
        metadata_io_.reset();
        pending_metadata_path_[0] = '\0';
        pending_metadata_bytes_.reset();
        metadata_save_pending_ = false;
        return true;
    }

    const OperationAdmission admission = metadata_io_.request_replace(
        pending_metadata_path_,
        pending_metadata_bytes_,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) {
        result = ExportStep::Waiting;
        return false;
    }
    if (admission != OperationAdmission::Accepted) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata save rejected path=%s\n",
                  pending_metadata_path_[0] ? pending_metadata_path_ : "--");
        pending_metadata_path_[0] = '\0';
        pending_metadata_bytes_.reset();
        metadata_save_pending_ = false;
        return true;
    }

    result = ExportStep::Waiting;
    return false;
}

void StorageSyncEngine::apply_config_locked(const SmbExportConfig &config) {
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
    metadata_file_.reset();
    metadata_loaded_ = false;
    if (status_.configured) {
        if (!build_endpoint_state_dir_locked(config_,
                                             state_dir_,
                                             sizeof(state_dir_),
                                             &endpoint_hash_)) {
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
        status_.state = StorageSyncState::Idle;
        status_.pending = false;
        pending_run_kind_ = RunKind::Manual;
        status_.pending_reason[0] = '\0';
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

void StorageSyncEngine::apply_pending_config_locked() {
    if (!pending_config_valid_) return;

    const SmbExportConfig pending = pending_config_;
    pending_config_valid_ = false;
    pending_config_ = SmbExportConfig();

    if (status_.state == StorageSyncState::Working) {
        preempt_run_locked();
    }
    apply_config_locked(pending);
}

void StorageSyncEngine::set_network_available(bool available) {
    const bool was_available = network_available_.exchange(available);
    if (was_available && !available) request_operation_abort();
    if (was_available == available) return;
    if (!lock(0)) return;
    status_.network_available = available;
    publish_runtime_locked();
    unlock();
}

void StorageSyncEngine::set_runtime_blocked(bool blocked) {
    const bool was_blocked = runtime_blocked_.exchange(blocked);
    if (!was_blocked && blocked) request_operation_abort();
}

void StorageSyncEngine::defer_idle_work_until(uint32_t until_ms) {
    idle_defer_until_ms_.store(until_ms);
}

void StorageSyncEngine::configure(const SmbExportConfig &config) {
    if (!lock(50)) return;
    if (!config_matches_locked(config)) {
        if (status_.state == StorageSyncState::Working) {
            pending_config_ = config;
            pending_config_valid_ = true;
            request_operation_abort();
        } else {
            apply_config_locked(config);
        }
    }
    unlock();
}

bool StorageSyncEngine::begin_run_locked() {
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
    phase_ = metadata_loaded_ ? WorkPhase::LoadInventory
                              : WorkPhase::LoadMetadata;
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] started reason=%s endpoint=%s\n",
              status_.pending_reason[0] ? status_.pending_reason : "manual",
              config_.endpoint);
    return true;
}

ExportStep StorageSyncEngine::step_load_metadata_locked() {
    const StorageFileClientResult io_result = metadata_file_.ready()
        ? StorageFileClientResult::Ready
        : metadata_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata load ignored path=%s/%s error=%s\n",
                  state_dir_[0] ? state_dir_ : "--",
                  SYNC_METADATA_FILE,
                  metadata_io_.error()[0] ? metadata_io_.error()
                                          : "storage_read_failed");
        metadata_io_.reset();
        metadata_file_.reset();
        metadata_loaded_ = true;
        phase_ = WorkPhase::LoadInventory;
        return ExportStep::Working;
    }
    if (io_result == StorageFileClientResult::Ready) {
        if (!metadata_file_.ready()) {
            metadata_file_ = metadata_io_.take_file();
        }

        char buffer[SYNC_METADATA_MAX_BYTES + 1] = {};
        size_t bytes_read = 0;
        bool read_complete = !metadata_file_.exists();
        if (metadata_file_.exists()) {
            const PreparedByteRead read = metadata_file_.read(
                0, reinterpret_cast<uint8_t *>(buffer), sizeof(buffer) - 1);
            if (read.state == PreparedByteReadState::Retry) {
                return ExportStep::Waiting;
            }
            read_complete = read.state == PreparedByteReadState::Data &&
                read.bytes == metadata_file_.size();
            bytes_read = read.bytes;
        }

        if (!read_complete) {
            Log::logf(CAT_STORAGE,
                      LOG_WARN,
                      "[SYNC] metadata load ignored path=%s/%s "
                      "error=short_read\n",
                      state_dir_[0] ? state_dir_ : "--",
                      SYNC_METADATA_FILE);
        } else {
            buffer[bytes_read] = '\0';
            parse_result_metadata_locked(buffer);
        }

        metadata_file_.reset();
        metadata_loaded_ = true;
        phase_ = WorkPhase::LoadInventory;
        return ExportStep::Working;
    }

    char path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    const int written = snprintf(path,
                                 sizeof(path),
                                 "%s/%s",
                                 state_dir_,
                                 SYNC_METADATA_FILE);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata load ignored error=path_too_long\n");
        metadata_loaded_ = true;
        phase_ = WorkPhase::LoadInventory;
        return ExportStep::Working;
    }

    const OperationAdmission admission = metadata_io_.request_read(
        path,
        SYNC_METADATA_MAX_BYTES,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata load ignored path=%s "
                  "error=storage_read_rejected\n",
                  path);
        metadata_loaded_ = true;
        phase_ = WorkPhase::LoadInventory;
        return ExportStep::Working;
    }

    return ExportStep::Waiting;
}

ExportStep StorageSyncEngine::step_load_inventory_locked() {
    if (!inventory_requested_) {
        const uint32_t generation = next_inventory_generation_;
        const OperationAdmission admission =
            inventory_loader_.request(state_dir_, generation);
        if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
        if (admission != OperationAdmission::Accepted) {
            fail_locked("export_inventory_rejected");
            return ExportStep::Idle;
        }

        next_inventory_generation_++;
        if (next_inventory_generation_ == 0) next_inventory_generation_ = 1;
        inventory_requested_ = true;
    }

    char error[AC_STORAGE_ERROR_MAX] = {};
    const StorageExportInventoryLoadResult result =
        inventory_loader_.poll(error, sizeof(error));
    if (result == StorageExportInventoryLoadResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (result == StorageExportInventoryLoadResult::Error) {
        fail_locked(error[0] ? error : "export_inventory_failed");
        return ExportStep::Idle;
    }

    export_inventory_ = inventory_loader_.snapshot();
    if (!export_inventory_) {
        fail_locked("export_inventory_missing");
        return ExportStep::Idle;
    }

    char planner_error[AC_STORAGE_ERROR_MAX] = {};
    if (!begin_export_planner_locked(planner_error, sizeof(planner_error))) {
        fail_locked(planner_error[0] ? planner_error : "planner_failed");
        return ExportStep::Idle;
    }

    phase_ = WorkPhase::Connect;
    return ExportStep::Working;
}

bool StorageSyncEngine::request_sync_with_kind(RunKind kind, const char *label) {
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
    return true;
}

bool StorageSyncEngine::request_manual_sync() {
    return request_sync_with_kind(RunKind::Manual, "manual");
}

bool StorageSyncEngine::request_startup_check() {
    return request_sync_with_kind(RunKind::StartupCheck, "startup_check");
}

bool StorageSyncEngine::request_verify_recent() {
    return request_sync_with_kind(RunKind::VerifyRecent, "verify_recent");
}

bool StorageSyncEngine::queue_post_therapy_locked(uint32_t now_ms) {
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

void StorageSyncEngine::queue_deferred_post_therapy_locked(uint32_t now_ms) {
    if (!post_therapy_requested_.load()) return;
    if (!status_.enabled || !status_.configured) {
        post_therapy_requested_.store(false);
        return;
    }
    if (status_.state == StorageSyncState::Working) return;
    post_therapy_requested_.store(false);
    (void)queue_post_therapy_locked(now_ms);
}

bool StorageSyncEngine::request_post_therapy_sync() {
    if (!lock(0)) {
        post_therapy_requested_.store(true);
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
    return true;
}

bool StorageSyncEngine::prepare_upload_buffer_locked() {
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

void StorageSyncEngine::release_upload_buffer_locked() {
    if (upload_buffer_) {
        Memory::free(upload_buffer_);
        upload_buffer_ = nullptr;
    }
    upload_buffer_size_ = 0;
}

bool StorageSyncEngine::operation_abort_cb(void *ctx) {
    StorageSyncEngine *job = static_cast<StorageSyncEngine *>(ctx);
    return !job || job->abort_requested_.load() ||
           job->runtime_blocked_.load();
}

BackgroundOperationControl StorageSyncEngine::operation_control() const {
    BackgroundOperationControl operation;
    operation.started_ms = millis();
    operation.timeout_ms = SMB_OPERATION_TIMEOUT_MS;
    operation.should_abort = &StorageSyncEngine::operation_abort_cb;
    operation.ctx = const_cast<StorageSyncEngine *>(this);
    return operation;
}

void StorageSyncEngine::request_operation_abort() {
    bool expected = false;
    if (abort_requested_.compare_exchange_strong(expected, true)) {
        operation_generation_.fetch_add(1);
    }
}

void StorageSyncEngine::close_latest_verify_locked() {
    latest_verify_ = LatestVerify();
}

bool StorageSyncEngine::begin_latest_verify_locked(char *error_out,
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

bool StorageSyncEngine::latest_verify_file_step_locked(char *error_out,
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

ExportStep StorageSyncEngine::publish_verify_latest_remote_locked(
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
    return ExportStep::Working;
}

bool StorageSyncEngine::next_file_locked() {
    if (!StorageService::status().available) {
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
                if (!schedule_completed_datalog_day_locked(
                        item.datalog_day)) {
                    fail_locked("done_marker_path_failed");
                    return false;
                }
                phase_ = !state_batch_.empty()
                    ? WorkPhase::FlushState
                    : pending_done_day_[0]
                        ? WorkPhase::WriteDoneMarker
                        : WorkPhase::NextFile;
                return true;
            }
            return plan_file_locked(item);
        case StorageExportPlannerResult::Yield:
            return true;
        case StorageExportPlannerResult::DecisionRequired:
            fail_locked("unexpected_planner_decision");
            return false;
        case StorageExportPlannerResult::Done:
            phase_ = !state_batch_.empty() ? WorkPhase::FlushState
                                           : WorkPhase::Finish;
            return true;
        case StorageExportPlannerResult::Error:
            fail_locked(error[0] ? error : "planner_failed");
            return false;
    }
    return true;
}

bool StorageSyncEngine::begin_export_planner_locked(char *error_out,
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

bool StorageSyncEngine::build_state_path_locked(const char *path,
                                             char *out,
                                             size_t out_size) const {
    if (!path || !out || out_size == 0 || !state_dir_[0]) return false;
    return storage_export_build_state_path(state_dir_,
                                           path,
                                           out,
                                           out_size);
}

uint32_t StorageSyncEngine::next_storage_generation_locked() {
    const uint32_t generation = next_storage_generation_;
    next_storage_generation_++;
    if (next_storage_generation_ == 0) next_storage_generation_ = 1;
    return generation;
}

bool StorageSyncEngine::queue_current_state_locked() {
    if (current_file_.completion == FileCompletion::None ||
        !state_batch_.add(current_file_.state_path,
                          current_file_.path,
                          current_file_.size,
                          current_file_.mtime)) {
        return false;
    }

    if (current_file_.completion == FileCompletion::Uploaded) {
        pending_state_uploaded_++;
    } else {
        pending_state_skipped_++;
    }

    const bool batch_state = current_file_.batch_state;
    clear_current_file_locked();
    phase_ = batch_state ? WorkPhase::NextFile : WorkPhase::FlushState;
    return true;
}

void StorageSyncEngine::commit_pending_file_counts_locked() {
    status_.files_uploaded += pending_state_uploaded_;
    status_.files_skipped += pending_state_skipped_;
    pending_state_uploaded_ = 0;
    pending_state_skipped_ = 0;
}

bool StorageSyncEngine::schedule_completed_datalog_day_locked(const char *day) {
    pending_done_day_[0] = '\0';
    if (run_kind_is_reconcile(current_run_kind_)) return true;
    if (!day || !day[0] || !export_inventory_) return true;
    if (!storage_export_datalog_day_finalized(
            day,
            export_inventory_->latest_datalog_day()) ||
        export_inventory_->datalog_day_done(day)) {
        return true;
    }

    if (strlen(day) != 8) return false;
    copy_cstr(pending_done_day_, sizeof(pending_done_day_), day);
    return true;
}

bool StorageSyncEngine::prepare_state_file_locked() {
    if (pending_state_bytes_) return pending_state_path_[0] != '\0';
    if (!export_inventory_) return false;

    const char *state_path = state_batch_.first_state_path();
    if (!state_path || !state_path[0]) return false;
    copy_cstr(pending_state_path_, sizeof(pending_state_path_), state_path);
    pending_state_bytes_ = state_batch_.build_file(*export_inventory_,
                                                   state_path);
    return pending_state_bytes_ != nullptr;
}

bool StorageSyncEngine::prepare_done_marker_locked() {
    if (pending_state_bytes_) return pending_state_path_[0] != '\0';
    if (!pending_done_day_[0] ||
        !storage_export_build_done_path(state_dir_,
                                        pending_done_day_,
                                        pending_state_path_,
                                        sizeof(pending_state_path_))) {
        return false;
    }

    static constexpr char DONE_MARKER[] = "done\n";
    pending_state_bytes_ = freeze_bytes(DONE_MARKER,
                                        sizeof(DONE_MARKER) - 1);
    return pending_state_bytes_ != nullptr;
}

bool StorageSyncEngine::remote_parent_dir_locked(const char *remote_path,
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

bool StorageSyncEngine::plan_file_locked(const StorageExportPlannerItem &item) {
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
    if (!stream_port_) {
        fail_locked("stream_port_unavailable");
        return false;
    }
    current_file_.local.configure(*stream_port_, path,
                                  current_file_.size,
                                  current_file_.mtime);

    if (!item.state_path[0]) {
        fail_locked("state_path_failed");
        return false;
    }
    copy_cstr(current_file_.state_path,
              sizeof(current_file_.state_path),
              item.state_path);
    current_file_.batch_state =
        item.state_write_mode == StorageExportStateWriteMode::Append;
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

void StorageSyncEngine::close_local_locked(bool complete) {
    current_file_.local.close(complete);
}

void StorageSyncEngine::clear_current_file_locked() {
    close_local_locked();
    current_file_ = CurrentFile();
    status_.current_path[0] = '\0';
}

void StorageSyncEngine::finish_run_locked() {
    close_latest_verify_locked();
    export_planner_.reset();
    inventory_loader_.reset();
    export_inventory_.reset();
    inventory_requested_ = false;
    release_upload_buffer_locked();
    state_io_.reset();
    state_batch_.clear();
    pending_state_path_[0] = '\0';
    pending_done_day_[0] = '\0';
    pending_state_bytes_.reset();
    pending_state_uploaded_ = 0;
    pending_state_skipped_ = 0;
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
    if (!queue_result_metadata_save_locked()) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata save queue failed path=%s/%s\n",
                  state_dir_[0] ? state_dir_ : "--",
                  SYNC_METADATA_FILE);
    }
    current_run_kind_ = RunKind::Manual;
    sync_after_verify_ = false;
    queue_deferred_post_therapy_locked(status_.updated_ms);
    publish_runtime_locked();
}

void StorageSyncEngine::preempt_run_locked() {
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

void StorageSyncEngine::fail_locked(const char *error) {
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
    state_io_.reset();
    state_batch_.clear();
    pending_state_path_[0] = '\0';
    pending_done_day_[0] = '\0';
    pending_state_bytes_.reset();
    pending_state_uploaded_ = 0;
    pending_state_skipped_ = 0;
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
    if (!queue_result_metadata_save_locked()) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] metadata save queue failed after error "
                  "path=%s/%s\n",
                  state_dir_[0] ? state_dir_ : "--",
                  SYNC_METADATA_FILE);
    }
    pending_run_kind_ = failed_kind;
    current_run_kind_ = RunKind::Manual;
    publish_runtime_locked();
}

void StorageSyncEngine::queue_retry_locked(uint32_t now_ms) {
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

void StorageSyncEngine::queue_reconcile_if_due_locked(uint32_t now_ms) {
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

bool StorageSyncEngine::prepare_step_locked(uint32_t now_ms, ExportStep &result) {
    apply_pending_config_locked();
    if (!service_result_metadata_save_locked(result)) return false;

    const uint32_t defer_until = idle_defer_until_ms_.load();
    if (defer_until != 0 &&
        static_cast<int32_t>(now_ms - defer_until) < 0 &&
        status_.state != StorageSyncState::Working) {
        result = status_.pending ? ExportStep::Waiting : ExportStep::Idle;
        return false;
    }
    queue_retry_locked(now_ms);
    queue_reconcile_if_due_locked(now_ms);
    queue_deferred_post_therapy_locked(now_ms);

    if (abort_requested_.load() &&
        status_.state == StorageSyncState::Working) {
        preempt_run_locked();
        result = status_.pending ? ExportStep::Waiting : ExportStep::Idle;
        return false;
    }

    if (runtime_blocked_.load()) {
        result = status_.pending ? ExportStep::Waiting : ExportStep::Idle;
        return false;
    }

    const bool ready =
        status_.enabled && status_.configured &&
        (status_.pending || status_.state == StorageSyncState::Working);
    if (!ready) {
        result = ExportStep::Idle;
        return false;
    }
    if (status_.pending && status_.state != StorageSyncState::Working &&
        !network_available_.load()) {
        status_.network_available = false;
        result = ExportStep::Idle;
        return false;
    }
    status_.network_available = network_available_.load();
    const StorageServiceStatus edf = StorageService::status();
    if (edf.busy || edf.edf_queued > 0 || edf.open_file_count > 0) {
        result = ExportStep::Waiting;
        return false;
    }
    if (status_.state != StorageSyncState::Working &&
        edf.maintenance_active) {
        result = ExportStep::Waiting;
        return false;
    }
    if (status_.state != StorageSyncState::Working &&
        !begin_run_locked()) {
        result = ExportStep::Idle;
        return false;
    }
    return true;
}

ExportStep StorageSyncEngine::step_verify_latest_start_locked(
    char *error_out,
    size_t error_out_size) {
    if (!begin_latest_verify_locked(error_out, error_out_size)) {
        fail_locked(error_out[0] ? error_out : "latest_verify_start_failed");
        return ExportStep::Idle;
    }
    return ExportStep::Working;
}

ExportStep StorageSyncEngine::step_verify_latest_file_locked(
    char *error_out,
    size_t error_out_size) {
    if (!latest_verify_file_step_locked(error_out, error_out_size)) {
        fail_locked(error_out[0] ? error_out : "latest_verify_failed");
        return ExportStep::Idle;
    }
    return ExportStep::Working;
}

ExportStep StorageSyncEngine::step_verify_latest_invalidate_locked() {
    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "state_invalidate_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        state_io_.reset();
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] invalidated latest state path=%s\n",
                  latest_verify_.state_path);
        close_latest_verify_locked();
        phase_ = WorkPhase::Finish;
        return ExportStep::Working;
    }

    const OperationAdmission admission = state_io_.request_remove(
        latest_verify_.state_path,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("state_invalidate_rejected");
        return ExportStep::Idle;
    }

    return ExportStep::Waiting;
}

ExportStep StorageSyncEngine::step_validate_local_locked() {
    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "local_stat_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        const StorageFileInfo info = state_io_.info();
        state_io_.reset();
        if (!info.exists || info.directory ||
            info.size != current_file_.size ||
            info.modified != current_file_.mtime) {
            fail_locked("local_changed");
            return ExportStep::Idle;
        }
        if (!queue_current_state_locked()) {
            fail_locked("state_batch_failed");
            return ExportStep::Idle;
        }

        return ExportStep::Working;
    }

    const OperationAdmission admission = state_io_.request_stat(
        current_file_.path,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("local_stat_rejected");
        return ExportStep::Idle;
    }

    return ExportStep::Waiting;
}

ExportStep StorageSyncEngine::step_flush_state_locked() {
    if (state_batch_.empty()) {
        commit_pending_file_counts_locked();
        phase_ = pending_done_day_[0] ? WorkPhase::WriteDoneMarker
                                      : WorkPhase::NextFile;
        return ExportStep::Working;
    }

    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "state_write_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        state_io_.reset();
        state_batch_.erase_state_path(pending_state_path_);
        pending_state_path_[0] = '\0';
        pending_state_bytes_.reset();
        if (state_batch_.empty()) {
            commit_pending_file_counts_locked();
            phase_ = pending_done_day_[0] ? WorkPhase::WriteDoneMarker
                                          : WorkPhase::NextFile;
        }
        return ExportStep::Working;
    }

    if (!prepare_state_file_locked()) {
        fail_locked("state_file_build_failed");
        return ExportStep::Idle;
    }
    const OperationAdmission admission = state_io_.request_replace(
        pending_state_path_,
        pending_state_bytes_,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("state_write_rejected");
        return ExportStep::Idle;
    }

    return ExportStep::Waiting;
}

ExportStep StorageSyncEngine::step_write_done_marker_locked() {
    if (!pending_done_day_[0]) {
        phase_ = WorkPhase::NextFile;
        return ExportStep::Working;
    }

    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "done_marker_write_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        state_io_.reset();
        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SYNC] DATALOG day complete day=%s\n",
                  pending_done_day_);
        pending_done_day_[0] = '\0';
        pending_state_path_[0] = '\0';
        pending_state_bytes_.reset();
        phase_ = WorkPhase::NextFile;
        return ExportStep::Working;
    }

    if (!prepare_done_marker_locked()) {
        fail_locked("done_marker_build_failed");
        return ExportStep::Idle;
    }
    const OperationAdmission admission = state_io_.request_replace(
        pending_state_path_,
        pending_state_bytes_,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("done_marker_write_rejected");
        return ExportStep::Idle;
    }

    return ExportStep::Waiting;
}

bool StorageSyncEngine::phase_has_blocking_io(WorkPhase phase) {
    switch (phase) {
        case WorkPhase::Connect:
        case WorkPhase::VerifyLatestRemote:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::EnsureRemoteDir:
        case WorkPhase::OpenLocal:
        case WorkPhase::OpenRemote:
        case WorkPhase::UploadChunk:
        case WorkPhase::CloseRemote:
        case WorkPhase::Finish:
            return true;

        case WorkPhase::Idle:
        case WorkPhase::LoadMetadata:
        case WorkPhase::LoadInventory:
        case WorkPhase::VerifyLatestStart:
        case WorkPhase::VerifyLatestFile:
        case WorkPhase::VerifyLatestInvalidate:
        case WorkPhase::NextFile:
        case WorkPhase::ValidateLocal:
        case WorkPhase::FlushState:
        case WorkPhase::WriteDoneMarker:
            return false;
    }
    return false;
}

void StorageSyncEngine::execute_blocking_phase(WorkPhase phase,
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

        case WorkPhase::OpenLocal:
            result.ok = current_file_.local.open(operation,
                                                  result.error,
                                                  sizeof(result.error));
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

            if (!current_file_.local.read_exact(upload_buffer_,
                                                to_read,
                                                operation,
                                                result.error,
                                                sizeof(result.error))) {
                return;
            }

            result.transferred = smb_.write(upload_buffer_,
                                            to_read,
                                            result.error,
                                            sizeof(result.error),
                                            &operation);
            result.ok = result.transferred >= 0 &&
                        static_cast<size_t>(result.transferred) == to_read;
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
        case WorkPhase::LoadMetadata:
        case WorkPhase::LoadInventory:
        case WorkPhase::VerifyLatestStart:
        case WorkPhase::VerifyLatestFile:
        case WorkPhase::VerifyLatestInvalidate:
        case WorkPhase::NextFile:
        case WorkPhase::ValidateLocal:
        case WorkPhase::FlushState:
        case WorkPhase::WriteDoneMarker:
            copy_cstr(result.error,
                      sizeof(result.error),
                      "invalid_blocking_phase");
            return;
    }
}

ExportStep StorageSyncEngine::publish_blocking_phase_locked(
    WorkPhase phase,
    const BlockingResult &result) {
    if (status_.state != StorageSyncState::Working || phase_ != phase) {
        smb_.abort_connection();
        return ExportStep::Idle;
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
        return status_.pending ? ExportStep::Waiting : ExportStep::Idle;
    }

    if (!result.ok) {
        fail_locked(result.error[0] ? result.error : "network_io_failed");
        return ExportStep::Idle;
    }

    switch (phase) {
        case WorkPhase::Connect:
            if (run_kind_is_verify(current_run_kind_) &&
                !run_kind_is_reconcile(current_run_kind_)) {
                phase_ = WorkPhase::VerifyLatestStart;
                return ExportStep::Working;
            }
            if (!prepare_upload_buffer_locked()) {
                fail_locked("upload_buffer_alloc");
                return ExportStep::Idle;
            }
            phase_ = WorkPhase::NextFile;
            return ExportStep::Working;

        case WorkPhase::VerifyLatestRemote:
            return publish_verify_latest_remote_locked(result.remote);

        case WorkPhase::ResolveRemoteFile:
            if (result.remote.exists && !result.remote.directory &&
                result.remote.size == current_file_.size) {
                if (current_file_.local_state_complete) {
                    status_.files_skipped++;
                    clear_current_file_locked();
                    phase_ = WorkPhase::NextFile;
                } else {
                    current_file_.completion = FileCompletion::Skipped;
                    phase_ = WorkPhase::ValidateLocal;
                }
                return ExportStep::Working;
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
            return ExportStep::Working;

        case WorkPhase::EnsureRemoteDir:
            copy_cstr(ensured_remote_dir_,
                      sizeof(ensured_remote_dir_),
                      current_file_.remote_dir);
            phase_ = WorkPhase::OpenLocal;
            return ExportStep::Working;

        case WorkPhase::OpenLocal:
            phase_ = WorkPhase::OpenRemote;
            return ExportStep::Working;

        case WorkPhase::OpenRemote:
            current_file_.offset = 0;
            phase_ = current_file_.size == 0
                ? WorkPhase::CloseRemote
                : WorkPhase::UploadChunk;
            return ExportStep::Working;

        case WorkPhase::UploadChunk:
            current_file_.offset +=
                static_cast<uint64_t>(result.transferred);
            status_.bytes_uploaded +=
                static_cast<uint64_t>(result.transferred);
            status_.updated_ms = nonzero_millis(millis());
            if (current_file_.offset >= current_file_.size) {
                phase_ = WorkPhase::CloseRemote;
            }
            return ExportStep::Working;

        case WorkPhase::CloseRemote:
            close_local_locked(true);
            current_file_.completion = FileCompletion::Uploaded;
            phase_ = WorkPhase::ValidateLocal;
            return ExportStep::Working;

        case WorkPhase::Finish:
            finish_run_locked();
            return ExportStep::Idle;

        case WorkPhase::Idle:
        case WorkPhase::LoadMetadata:
        case WorkPhase::LoadInventory:
        case WorkPhase::VerifyLatestStart:
        case WorkPhase::VerifyLatestFile:
        case WorkPhase::VerifyLatestInvalidate:
        case WorkPhase::NextFile:
        case WorkPhase::ValidateLocal:
        case WorkPhase::FlushState:
        case WorkPhase::WriteDoneMarker:
            fail_locked("invalid_blocking_phase");
            return ExportStep::Idle;
    }
    return ExportStep::Idle;
}

ExportStep StorageSyncEngine::step_work_phase_locked() {
    char error[AC_STORAGE_ERROR_MAX] = {};
    switch (phase_) {
        case WorkPhase::Idle:
            phase_ = metadata_loaded_ ? WorkPhase::LoadInventory
                                      : WorkPhase::LoadMetadata;
            return ExportStep::Working;

        case WorkPhase::LoadMetadata:
            return step_load_metadata_locked();

        case WorkPhase::LoadInventory:
            return step_load_inventory_locked();

        case WorkPhase::Connect:
        case WorkPhase::VerifyLatestRemote:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::EnsureRemoteDir:
        case WorkPhase::OpenLocal:
        case WorkPhase::OpenRemote:
        case WorkPhase::UploadChunk:
        case WorkPhase::CloseRemote:
        case WorkPhase::Finish:
            return ExportStep::Waiting;

        case WorkPhase::VerifyLatestStart:
            return step_verify_latest_start_locked(error, sizeof(error));

        case WorkPhase::VerifyLatestFile:
            return step_verify_latest_file_locked(error, sizeof(error));

        case WorkPhase::VerifyLatestInvalidate:
            return step_verify_latest_invalidate_locked();

        case WorkPhase::NextFile:
            return next_file_locked() ? ExportStep::Working : ExportStep::Idle;

        case WorkPhase::ValidateLocal:
            return step_validate_local_locked();

        case WorkPhase::FlushState:
            return step_flush_state_locked();

        case WorkPhase::WriteDoneMarker:
            return step_write_done_marker_locked();
    }
    return ExportStep::Idle;
}

ExportStep StorageSyncEngine::step() {
    if (!lock(20)) return ExportStep::Waiting;
    ExportStep result = ExportStep::Idle;
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
        return ExportStep::Idle;
    }
    result = publish_blocking_phase_locked(phase, blocking_result);
    unlock();
    return result;
}

StorageSyncStatus StorageSyncEngine::status() const {
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

StorageSyncRuntimeStatus StorageSyncEngine::runtime_status() const {
    StorageSyncRuntimeStatus out;
    out.state = static_cast<StorageSyncState>(runtime_state_.load());
    out.pending = runtime_pending_.load();
    out.enabled = runtime_enabled_.load();
    out.configured = runtime_configured_.load();
    out.network_available = network_available_.load();
    return out;
}

}  // namespace aircannect
