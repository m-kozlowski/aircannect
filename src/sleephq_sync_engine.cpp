#include "sleephq_sync_engine.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ArduinoJson.h>

#include "crc32.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "runtime_clock.h"
#include "storage_export_plan.h"
#include "storage_service.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t SLEEPHQ_IMPORT_POLL_INTERVAL_MS = 2000;
// SleepHQ import processing is asynchronous server-side work. Per-day EDF
// imports can stay in "unpacking" briefly after upload completes, but a long
// wait hides a stuck server-side import.
static constexpr uint32_t SLEEPHQ_IMPORT_POLL_TIMEOUT_MS =
    5UL * 60UL * 1000UL;
static constexpr uint32_t SLEEPHQ_API_OPERATION_TIMEOUT_MS = 30UL * 1000UL;
static constexpr uint32_t SLEEPHQ_UPLOAD_MIN_TIMEOUT_MS = 60UL * 1000UL;
static constexpr uint32_t SLEEPHQ_UPLOAD_MAX_TIMEOUT_MS =
    10UL * 60UL * 1000UL;
static constexpr uint32_t SLEEPHQ_UPLOAD_MIN_BYTES_PER_SECOND = 8UL * 1024UL;
static constexpr uint32_t SLEEPHQ_RETRY_BACKOFF_MS[] = {
    15UL * 60UL * 1000UL,
    60UL * 60UL * 1000UL,
    6UL * 60UL * 60UL * 1000UL,
};
static constexpr const char *SLEEPHQ_INFLIGHT_FILE = "inflight.state";
static constexpr size_t SLEEPHQ_INFLIGHT_MAX_BYTES = 512 * 1024;
static constexpr size_t SLEEPHQ_INFLIGHT_READ_STEP_BYTES = 512;
static constexpr size_t SLEEPHQ_REBUILD_MARKER_MAX_BYTES = 31;
static constexpr uint32_t SLEEPHQ_POST_THERAPY_DATALOG_DAY_LIMIT = 1;
static constexpr uint32_t SLEEPHQ_REMOTE_FILE_PER_PAGE = 50;
static constexpr uint32_t SLEEPHQ_REMOTE_FILE_LOOKUP_LIMIT = 500;
static constexpr uint32_t SLEEPHQ_REMOTE_FILE_PAGE_LIMIT =
    (SLEEPHQ_REMOTE_FILE_LOOKUP_LIMIT + SLEEPHQ_REMOTE_FILE_PER_PAGE - 1) /
    SLEEPHQ_REMOTE_FILE_PER_PAGE;
static constexpr const char *SLEEPHQ_IDENTIFICATION_PATH =
    "/Identification.json";
static constexpr size_t SLEEPHQ_IDENTIFICATION_JSON_MAX = 8192;
static constexpr uint32_t SLEEPHQ_REMOTE_MACHINE_PER_PAGE = 25;
static constexpr uint32_t SLEEPHQ_REMOTE_MACHINE_LOOKUP_LIMIT = 250;
static constexpr uint32_t SLEEPHQ_REMOTE_MACHINE_PAGE_LIMIT =
    (SLEEPHQ_REMOTE_MACHINE_LOOKUP_LIMIT + SLEEPHQ_REMOTE_MACHINE_PER_PAGE -
     1) /
    SLEEPHQ_REMOTE_MACHINE_PER_PAGE;
static constexpr uint32_t SLEEPHQ_DATALOG_REBUILD_COOLDOWN_SECONDS =
    6UL * 60UL * 60UL;

bool parse_uint64_text(const char *text, uint64_t &out) {
    if (!text || !text[0]) return false;

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

uint32_t sleep_hq_upload_timeout_ms(uint64_t size) {
    const uint64_t transfer_ms =
        (size * 1000ULL + SLEEPHQ_UPLOAD_MIN_BYTES_PER_SECOND - 1) /
        SLEEPHQ_UPLOAD_MIN_BYTES_PER_SECOND;
    uint64_t timeout_ms = transfer_ms + SLEEPHQ_API_OPERATION_TIMEOUT_MS;
    if (timeout_ms < SLEEPHQ_UPLOAD_MIN_TIMEOUT_MS) {
        timeout_ms = SLEEPHQ_UPLOAD_MIN_TIMEOUT_MS;
    }
    if (timeout_ms > SLEEPHQ_UPLOAD_MAX_TIMEOUT_MS) {
        timeout_ms = SLEEPHQ_UPLOAD_MAX_TIMEOUT_MS;
    }
    return static_cast<uint32_t>(timeout_ms);
}

bool parse_http_error_code(const char *error, int &code) {
    code = 0;
    if (!error || strncmp(error, "http_", 5) != 0) return false;
    char *end = nullptr;
    const long value = strtol(error + 5, &end, 10);
    if (!end || *end != '\0' || value <= 0 || value > 999) {
        return false;
    }
    code = static_cast<int>(value);
    return true;
}

bool sleep_hq_error_retryable(const char *error) {
    if (!error || !*error) return true;
    int http_code = 0;
    if (parse_http_error_code(error, http_code)) {
        if (http_code == 408 || http_code == 429 || http_code >= 500) {
            return true;
        }
        return http_code == 404;
    }

    static const char *const PERMANENT[] = {
        "bad_attach_request",
        "bad_child_path",
        "bad_upload_request",
        "import_failed",
        "import_id_missing",
        "multipart_size_failed",
        "not_configured",
        "request_header_too_long",
        "sleep_path_failed",
        "state_path_failed",
        "team_id_missing",
        "token_json_parse",
        "token_missing",
    };
    for (const char *item : PERMANENT) {
        if (strcmp(error, item) == 0) return false;
    }
    return true;
}

bool sleep_hq_error_preserves_retry_attempt(const char *error) {
    int http_code = 0;
    return parse_http_error_code(error, http_code) && http_code == 502;
}

bool datalog_day_to_iso_date(const char *day, char *out, size_t out_size) {
    if (!day || !out || out_size < 11) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (day[i] < '0' || day[i] > '9') return false;
    }
    if (day[8] != '\0') return false;
    out[0] = day[0];
    out[1] = day[1];
    out[2] = day[2];
    out[3] = day[3];
    out[4] = '-';
    out[5] = day[4];
    out[6] = day[5];
    out[7] = '-';
    out[8] = day[6];
    out[9] = day[7];
    out[10] = '\0';
    return true;
}

const char *json_serial_or_empty(JsonDocument &doc) {
    const char *serial =
        doc["FlowGenerator"]["IdentificationProfiles"]["Product"]
           ["SerialNumber"] |
        "";
    if (serial[0]) return serial;
    return doc["IdentificationProfiles"]["Product"]["SerialNumber"] | "";
}

}  // namespace

const char *sleephq_sync_state_name(SleepHqSyncState state) {
    switch (state) {
        case SleepHqSyncState::Disabled: return "disabled";
        case SleepHqSyncState::Idle: return "idle";
        case SleepHqSyncState::Pending: return "pending";
        case SleepHqSyncState::Working: return "working";
        case SleepHqSyncState::Error: return "error";
    }
    return "unknown";
}

bool SleepHqSyncEngine::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void SleepHqSyncEngine::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void SleepHqSyncEngine::publish_runtime_locked() {
    const uint32_t generation = status_.config_generation;
    runtime_state_.store(static_cast<uint8_t>(status_.state));
    runtime_pending_.store(status_.pending);
    runtime_configured_.store(status_.configured);
    runtime_config_generation_.store(generation);
    runtime_team_id_.store(status_.team_id);
    runtime_completed_check_generation_.store(
        status_.team_id != 0 && status_.last_check_epoch != 0
            ? generation
            : 0);
}

bool SleepHqSyncEngine::snapshot_configured(const SleepHqExportConfig &config) {
    return config.client_id[0] && config.client_secret[0];
}

SleepHqConfig SleepHqSyncEngine::client_config_from_snapshot(
    const SleepHqExportConfig &config) {
    SleepHqConfig out;
    copy_cstr(out.client_id, sizeof(out.client_id), config.client_id);
    copy_cstr(out.client_secret, sizeof(out.client_secret),
              config.client_secret);
    copy_cstr(out.team_id, sizeof(out.team_id), config.team_id);
    copy_cstr(out.device_id, sizeof(out.device_id), config.device_id);
    return out;
}

const char *SleepHqSyncEngine::inflight_phase_name(InflightPhase phase) {
    switch (phase) {
        case InflightPhase::Uploading: return "uploading";
        case InflightPhase::Processing: return "processing";
        case InflightPhase::None: break;
    }
    return "none";
}

bool SleepHqSyncEngine::parse_inflight_phase(const char *text,
                                          InflightPhase &out) {
    if (!text) return false;
    if (strcmp(text, "uploading") == 0) {
        out = InflightPhase::Uploading;
        return true;
    }
    if (strcmp(text, "processing") == 0) {
        out = InflightPhase::Processing;
        return true;
    }
    if (strcmp(text, "none") == 0) {
        out = InflightPhase::None;
        return true;
    }
    return false;
}

void SleepHqSyncEngine::begin(const SleepHqExportConfig &config,
                           StorageScanPort &scan_port,
                           StorageReadPort &read_port,
                           StorageStreamPort &stream_port,
                           StorageAtomicWritePort &write_port,
                           StoragePathPort &path_port) {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    inventory_loader_.begin(scan_port, read_port);
    state_io_.begin(read_port, write_port, path_port);
    stream_port_ = &stream_port;
    configure(config);
}

bool SleepHqSyncEngine::config_matches_locked(
    const SleepHqExportConfig &config) const {
    const SleepHqExportConfig &active =
        pending_config_valid_ ? pending_config_ : config_;
    return strcmp(active.client_id, config.client_id) == 0 &&
           strcmp(active.client_secret, config.client_secret) == 0 &&
           strcmp(active.team_id, config.team_id) == 0 &&
           strcmp(active.device_id, config.device_id) == 0;
}

void SleepHqSyncEngine::apply_config_locked(const SleepHqExportConfig &config) {
    config_ = config;
    status_.config_generation++;
    status_.configured = snapshot_configured(config_);
    status_.team_id = 0;
    status_.pending = false;
    status_.pending_reason[0] = 0;
    status_.last_error[0] = 0;
    state_dir_[0] = 0;
    inventory_loader_.reset();
    export_inventory_.reset();
    inventory_requested_ = false;
    state_io_.reset();
    state_batch_.clear();
    reset_inflight_reader_locked();
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    status_.state = status_.configured ? SleepHqSyncState::Idle
                                       : SleepHqSyncState::Disabled;
    phase_ = WorkPhase::Idle;
    publish_runtime_locked();
}

void SleepHqSyncEngine::apply_pending_config_locked() {
    if (!pending_config_valid_) return;

    const SleepHqExportConfig pending = pending_config_;
    pending_config_valid_ = false;
    pending_config_ = SleepHqExportConfig();

    if (status_.state == SleepHqSyncState::Working) {
        fail_locked("preempted");
    }
    apply_config_locked(pending);
}

void SleepHqSyncEngine::configure(const SleepHqExportConfig &config) {
    if (!lock(50)) return;

    if (!config_matches_locked(config)) {
        if (status_.state == SleepHqSyncState::Working) {
            pending_config_ = config;
            pending_config_valid_ = true;
            request_operation_abort();
        } else {
            apply_config_locked(config);
        }
    }
    unlock();
}

void SleepHqSyncEngine::set_network_available(bool available) {
    const bool was_available = network_available_.exchange(available);
    if (was_available && !available) request_operation_abort();
    if (!lock(0)) return;
    status_.network_available = available;
    publish_runtime_locked();
    unlock();
}

void SleepHqSyncEngine::set_runtime_blocked(bool blocked) {
    const bool was_blocked = runtime_blocked_.exchange(blocked);
    if (!was_blocked && blocked) request_operation_abort();
}

bool SleepHqSyncEngine::request_locked(RunKind kind,
                                    const char *reason,
                                    const char *datalog_day) {
    if (!status_.configured) {
        status_.state = SleepHqSyncState::Disabled;
        return false;
    }
    if (datalog_day && !storage_export_is_datalog_day_name(datalog_day)) {
        return false;
    }
    if (status_.state == SleepHqSyncState::Working) return false;
    pending_run_kind_ = kind;
    pending_datalog_day_[0] = '\0';
    if (datalog_day) {
        copy_cstr(pending_datalog_day_, sizeof(pending_datalog_day_),
                  datalog_day);
    }
    status_.pending = true;
    status_.state = SleepHqSyncState::Pending;
    copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
              reason && *reason ? reason : "manual");
    status_.last_error[0] = 0;
    status_.updated_ms = nonzero_millis(millis());
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    return true;
}

bool SleepHqSyncEngine::request_check(const char *reason) {
    if (!lock(0)) return false;
    const bool queued = request_locked(RunKind::Check, reason);
    publish_runtime_locked();
    unlock();
    return queued;
}

bool SleepHqSyncEngine::request_sync(const char *reason) {
    if (!lock(0)) return false;
    const bool queued = request_locked(RunKind::Sync, reason);
    publish_runtime_locked();
    unlock();
    if (queued) {
        Log::logf(CAT_SLEEPHQ, LOG_INFO, "sync queued reason=%s\n",
                  reason && *reason ? reason : "manual");
    }
    return queued;
}

bool SleepHqSyncEngine::request_sync_day(const char *day, const char *reason) {
    if (!day || !storage_export_is_datalog_day_name(day)) return false;
    if (!lock(0)) return false;
    const bool queued = request_locked(RunKind::Sync, reason, day);
    publish_runtime_locked();
    unlock();
    if (queued) {
        Log::logf(CAT_SLEEPHQ, LOG_INFO,
                  "sync queued reason=%s day=%s\n",
                  reason && *reason ? reason : "manual_day",
                  day);
    }
    return queued;
}

bool SleepHqSyncEngine::request_post_therapy_sync() {
    if (!lock(0)) return false;
    const bool queued = request_locked(RunKind::PostTherapySync,
                                       "post_therapy");
    publish_runtime_locked();
    unlock();
    if (queued) {
        Log::logf(CAT_SLEEPHQ, LOG_INFO,
                  "sync queued reason=post_therapy scope=latest_day\n");
    }
    return queued;
}

bool SleepHqSyncEngine::build_endpoint_state_dir_locked(uint32_t team_id,
                                                     char *out,
                                                     size_t out_size) const {
    if (!out || out_size == 0 || !team_id || !config_.client_id[0]) {
        return false;
    }
    uint32_t crc = crc32_ieee_initial_state();
    char team_text[16];
    snprintf(team_text, sizeof(team_text), "%lu",
             static_cast<unsigned long>(team_id));
    storage_export_hash_update_cstr(crc, team_text);
    storage_export_hash_update_cstr(crc, "\n");
    storage_export_hash_update_cstr(crc, config_.client_id);
    const uint32_t hash = crc32_ieee_finish_state(crc);
    const int written = snprintf(out, out_size,
                                 "/aircannect/sync/sleephq/%08lx",
                                 static_cast<unsigned long>(hash));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool SleepHqSyncEngine::begin_run_locked(uint32_t now_ms) {
    if (!status_.configured) {
        status_.state = SleepHqSyncState::Disabled;
        status_.pending = false;
        publish_runtime_locked();
        return false;
    }
    if (!network_available_.load()) {
        status_.network_available = false;
        return false;
    }
    if (runtime_blocked_.load()) return false;

    const RunKind kind = pending_run_kind_;
    char reason[AC_SLEEPHQ_SYNC_REASON_MAX] = {};
    char datalog_day[9] = {};
    copy_cstr(reason, sizeof(reason),
              status_.pending_reason[0] ? status_.pending_reason : "manual");
    copy_cstr(datalog_day, sizeof(datalog_day), pending_datalog_day_);

    reset_run_locked(false);
    current_run_kind_ = kind;
    copy_cstr(current_datalog_day_filter_,
              sizeof(current_datalog_day_filter_),
              datalog_day);
    retry_due_ms_ = 0;
    status_.state = SleepHqSyncState::Working;
    status_.pending = false;
    copy_cstr(status_.pending_reason, sizeof(status_.pending_reason), reason);
    status_.started_ms = now_ms;
    status_.updated_ms = now_ms;
    status_.last_error[0] = 0;
    status_.current_path[0] = 0;
    status_.files_seen = 0;
    status_.files_uploaded = 0;
    status_.files_skipped = 0;
    status_.files_failed = 0;
    status_.bytes_uploaded = 0;
    status_.import_id = 0;
    status_.import_status[0] = 0;
    abort_requested_.store(false);
    phase_ = WorkPhase::Connect;
    publish_runtime_locked();
    Log::logf(CAT_SLEEPHQ, LOG_INFO, "started reason=%s mode=%s\n",
              status_.pending_reason[0] ? status_.pending_reason : "manual",
              current_run_kind_ == RunKind::Check
                  ? "check"
                  : current_run_kind_ == RunKind::PostTherapySync
                        ? "post_therapy_sync"
                        : "sync");
    return true;
}

ExportStep SleepHqSyncEngine::step_load_inventory_locked() {
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

    char error[AC_SLEEPHQ_ERROR_MAX] = {};
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
    if (!prepare_remote_reconcile_locked(error, sizeof(error))) {
        fail_locked(error[0] ? error : "remote_reconcile_failed");
        return ExportStep::Idle;
    }

    phase_ = WorkPhase::ReadIdentification;
    return ExportStep::Working;
}

void SleepHqSyncEngine::clear_current_file_locked() {
    current_file_.reset();
    status_.current_path[0] = 0;
}

void SleepHqSyncEngine::clear_staged_locked() {
    if (staged_) {
        for (size_t i = 0; i < staged_capacity_; ++i) {
            staged_[i].~StagedFile();
        }
        Memory::free(staged_);
    }
    staged_ = nullptr;
    staged_count_ = 0;
    staged_capacity_ = 0;
}

void SleepHqSyncEngine::clear_remote_files_locked() {
    remote_files_.clear();
    remote_file_next_page_ = 1;
    remote_file_pages_loaded_ = 0;
    remote_file_cache_complete_ = false;
}

bool SleepHqSyncEngine::reserve_remote_dates_locked(size_t needed) {
    if (needed <= remote_date_capacity_) return true;
    size_t next = remote_date_capacity_ == 0 ? 16 : remote_date_capacity_ * 2;
    while (next < needed) next *= 2;
    RemoteMachineDateCache *items = static_cast<RemoteMachineDateCache *>(
        Memory::alloc_large(sizeof(RemoteMachineDateCache) * next, false));
    if (!items) {
        Log::logf(CAT_SLEEPHQ, LOG_ERROR,
                  "remote date cache allocation failed entries=%u bytes=%u\n",
                  static_cast<unsigned>(next),
                  static_cast<unsigned>(sizeof(RemoteMachineDateCache) *
                                        next));
        return false;
    }
    for (size_t i = 0; i < next; ++i) {
        new (&items[i]) RemoteMachineDateCache();
    }
    for (size_t i = 0; i < remote_date_count_; ++i) {
        items[i] = remote_dates_[i];
    }
    if (remote_dates_) Memory::free(remote_dates_);
    remote_dates_ = items;
    remote_date_capacity_ = next;
    return true;
}

bool SleepHqSyncEngine::cache_remote_date_locked(const char *day, bool exists) {
    if (!day || !day[0]) return false;
    for (size_t i = 0; i < remote_date_count_; ++i) {
        RemoteMachineDateCache &entry = remote_dates_[i];
        if (strcmp(entry.day, day) == 0) {
            entry.exists = exists;
            return true;
        }
    }
    if (!reserve_remote_dates_locked(remote_date_count_ + 1)) return false;
    RemoteMachineDateCache &entry = remote_dates_[remote_date_count_++];
    copy_cstr(entry.day, sizeof(entry.day), day);
    entry.exists = exists;
    return true;
}

bool SleepHqSyncEngine::cached_remote_date_exists_locked(const char *day,
                                                      bool &exists) const {
    if (!day || !day[0]) return false;
    for (size_t i = 0; i < remote_date_count_; ++i) {
        const RemoteMachineDateCache &entry = remote_dates_[i];
        if (strcmp(entry.day, day) == 0) {
            exists = entry.exists;
            return true;
        }
    }
    return false;
}

void SleepHqSyncEngine::clear_remote_dates_locked() {
    if (remote_dates_) {
        for (size_t i = 0; i < remote_date_capacity_; ++i) {
            remote_dates_[i].~RemoteMachineDateCache();
        }
        Memory::free(remote_dates_);
    }
    remote_dates_ = nullptr;
    remote_date_count_ = 0;
    remote_date_capacity_ = 0;
    remote_machine_id_ = 0;
    remote_machine_next_page_ = 1;
    remote_machine_pages_loaded_ = 0;
    remote_reconcile_enabled_ = false;
    remote_reconcile_all_missing_ = false;
    remote_serial_[0] = '\0';
}

bool SleepHqSyncEngine::build_datalog_rebuild_marker_path_locked(
    const char *day,
    char *out,
    size_t out_size) const {
    if (!state_dir_[0] || !day || strlen(day) != 8 ||
        !storage_export_all_digits(day, 8) || !out || out_size == 0) {
        return false;
    }
    const int written = snprintf(out, out_size, "%s/%s.rebuild",
                                 state_dir_, day);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool SleepHqSyncEngine::begin_remote_missing_datalog_day_locked(
    char *error,
    size_t error_size) {
    if (!pending_remote_day_[0]) {
        copy_cstr(error, error_size, "bad_datalog_day");
        return false;
    }
    if (!pending_remote_day_local_complete_) {
        return resolve_remote_missing_datalog_day_locked(false, 0,
                                                         error, error_size);
    }

    pending_state_path_[0] = '\0';
    pending_state_bytes_.reset();
    state_io_.reset();
    phase_ = WorkPhase::ReadRebuildMarker;
    return true;
}

bool SleepHqSyncEngine::resolve_remote_missing_datalog_day_locked(
    bool marker_recent,
    uint64_t marker_epoch,
    char *error,
    size_t error_size) {
    if (!pending_remote_day_[0]) {
        copy_cstr(error, error_size, "bad_datalog_day");
        return false;
    }

    const bool force_export = !marker_recent;
    if (marker_recent) {
        Log::logf(CAT_SLEEPHQ, LOG_DEBUG,
                  "remote machine-date still missing day=%s serial=%s; "
                  "rebuild suppressed marker_epoch=%llu\n",
                  pending_remote_day_,
                  remote_serial_,
                  static_cast<unsigned long long>(marker_epoch));
    } else {
        if (pending_remote_day_local_complete_) {
            copy_cstr(pending_rebuild_day_, sizeof(pending_rebuild_day_),
                      pending_remote_day_);
        }
        Log::logf(CAT_SLEEPHQ, LOG_INFO,
                  "remote machine-date missing day=%s serial=%s; "
                  "rebuilding day\n",
                  pending_remote_day_,
                  remote_serial_);
    }

    if (!export_planner_.resolve_datalog_day_decision(
            force_export, error, error_size)) {
        return false;
    }

    pending_remote_day_[0] = '\0';
    pending_remote_day_local_complete_ = false;
    phase_ = WorkPhase::NextFile;
    return true;
}

ExportStep SleepHqSyncEngine::step_read_rebuild_marker_locked() {
    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }

    bool marker_recent = false;
    uint64_t marker_epoch = 0;
    if (io_result == StorageFileClientResult::Error) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "remote rebuild marker read failed day=%s error=%s\n",
                  pending_remote_day_,
                  state_io_.error()[0] ? state_io_.error()
                                       : "storage_read_failed");
        state_io_.reset();
    } else if (io_result == StorageFileClientResult::Ready) {
        StoragePreparedFile marker = state_io_.take_file();
        if (marker.exists() && marker.size() != 0 &&
            marker.size() <= SLEEPHQ_REBUILD_MARKER_MAX_BYTES) {
            char raw[SLEEPHQ_REBUILD_MARKER_MAX_BYTES + 1] = {};
            const size_t read = marker.read(
                0, reinterpret_cast<uint8_t *>(raw), marker.size());
            if (read == marker.size()) {
                raw[read] = '\0';
                char *newline = strpbrk(raw, "\r\n");
                if (newline) *newline = '\0';
                if (parse_uint64_text(raw, marker_epoch)) {
                    const uint64_t now_epoch =
                        storage_export_current_epoch_seconds_or_zero();
                    marker_recent = now_epoch != 0 &&
                        (marker_epoch > now_epoch ||
                         now_epoch - marker_epoch <
                             SLEEPHQ_DATALOG_REBUILD_COOLDOWN_SECONDS);
                }
            }
        }
    } else {
        if (!build_datalog_rebuild_marker_path_locked(
                pending_remote_day_, pending_state_path_,
                sizeof(pending_state_path_))) {
            fail_locked("rebuild_marker_path_failed");
            return ExportStep::Idle;
        }

        const OperationAdmission admission = state_io_.request_read(
            pending_state_path_, SLEEPHQ_REBUILD_MARKER_MAX_BYTES,
            next_storage_generation_locked());
        if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
        if (admission != OperationAdmission::Accepted) {
            fail_locked("rebuild_marker_read_rejected");
            return ExportStep::Idle;
        }
        return ExportStep::Waiting;
    }

    pending_state_path_[0] = '\0';
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    if (!resolve_remote_missing_datalog_day_locked(
            marker_recent, marker_epoch, error, sizeof(error))) {
        fail_locked(error[0] ? error : "planner_decision_failed");
        return ExportStep::Idle;
    }
    return ExportStep::Working;
}

void SleepHqSyncEngine::reset_run_locked(bool keep_status) {
    client_.disconnect();
    inventory_loader_.reset();
    export_inventory_.reset();
    export_planner_.reset();
    clear_current_file_locked();
    clear_staged_locked();
    clear_remote_files_locked();
    clear_remote_dates_locked();
    state_batch_.clear();
    state_io_.reset();
    reset_inflight_reader_locked();
    phase_ = WorkPhase::Idle;
    import_batch_active_ = false;
    import_process_started_ms_ = 0;
    import_poll_due_ms_ = 0;
    staged_validation_index_ = 0;
    inflight_phase_ = InflightPhase::None;
    pending_inflight_phase_ = InflightPhase::None;
    inflight_remove_action_ = InflightRemoveAction::None;
    storage_next_phase_ = WorkPhase::Idle;
    storage_failure_[0] = '\0';
    pending_rebuild_day_[0] = '\0';
    pending_done_day_[0] = '\0';
    pending_remote_day_[0] = '\0';
    pending_remote_day_local_complete_ = false;
    pending_datalog_day_[0] = '\0';
    current_datalog_day_filter_[0] = '\0';
    state_dir_[0] = 0;
    pending_state_path_[0] = '\0';
    pending_state_bytes_.reset();
    inventory_requested_ = false;
    current_run_kind_ = RunKind::Check;
    abort_requested_.store(false);
    if (!keep_status) {
        const bool configured = status_.configured;
        const bool network_available = status_.network_available;
        const uint32_t generation = status_.config_generation;
        const uint32_t team_id = status_.team_id;
        const uint64_t last_check = status_.last_check_epoch;
        const uint64_t last_sync = status_.last_sync_epoch;
        const uint32_t last_sync_seen = status_.last_sync_files_seen;
        const uint32_t last_sync_uploaded = status_.last_sync_files_uploaded;
        const uint32_t last_sync_failed = status_.last_sync_files_failed;
        const uint64_t last_sync_bytes = status_.last_sync_bytes_uploaded;
        const uint64_t last_failure = status_.last_failure_epoch;
        status_ = SleepHqSyncStatus();
        status_.configured = configured;
        status_.network_available = network_available;
        status_.config_generation = generation;
        status_.team_id = last_check != 0 ? team_id : 0;
        status_.last_check_epoch = last_check;
        status_.last_sync_epoch = last_sync;
        status_.last_sync_files_seen = last_sync_seen;
        status_.last_sync_files_uploaded = last_sync_uploaded;
        status_.last_sync_files_failed = last_sync_failed;
        status_.last_sync_bytes_uploaded = last_sync_bytes;
        status_.last_failure_epoch = last_failure;
    }
}

void SleepHqSyncEngine::finish_check_locked(uint32_t team_id) {
    status_.team_id = team_id;
    status_.last_check_epoch = storage_export_current_epoch_seconds_or_zero();
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    status_.state = status_.configured ? SleepHqSyncState::Idle
                                       : SleepHqSyncState::Disabled;
    status_.pending_reason[0] = 0;
    status_.updated_ms = nonzero_millis(millis());
    Log::logf(CAT_SLEEPHQ, LOG_INFO, "check ok team_id=%lu\n",
              static_cast<unsigned long>(team_id));
    reset_run_locked(true);
    publish_runtime_locked();
}

void SleepHqSyncEngine::finish_sync_locked() {
    status_.last_sync_epoch = storage_export_current_epoch_seconds_or_zero();
    status_.last_sync_files_seen = status_.files_seen;
    status_.last_sync_files_uploaded = status_.files_uploaded;
    status_.last_sync_files_failed = status_.files_failed;
    status_.last_sync_bytes_uploaded = status_.bytes_uploaded;
    if (status_.last_sync_epoch != 0 &&
        status_.last_sync_epoch > status_.last_check_epoch) {
        status_.last_check_epoch = status_.last_sync_epoch;
    }
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    status_.state = status_.configured ? SleepHqSyncState::Idle
                                       : SleepHqSyncState::Disabled;
    status_.pending_reason[0] = 0;
    status_.updated_ms = nonzero_millis(millis());
    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "done seen=%u uploaded=%u skipped=%u failed=%u bytes=%llu\n",
              static_cast<unsigned>(status_.files_seen),
              static_cast<unsigned>(status_.files_uploaded),
              static_cast<unsigned>(status_.files_skipped),
              static_cast<unsigned>(status_.files_failed),
              static_cast<unsigned long long>(status_.bytes_uploaded));
    reset_run_locked(true);
    publish_runtime_locked();
}

void SleepHqSyncEngine::fail_locked(const char *error) {
    if (error && strcmp(error, "preempted") == 0) {
        const RunKind kind = current_run_kind_;
        char reason[AC_SLEEPHQ_SYNC_REASON_MAX] = {};
        char datalog_day[9] = {};
        copy_cstr(reason, sizeof(reason),
                  status_.pending_reason[0]
                      ? status_.pending_reason
                      : "preempted");
        copy_cstr(datalog_day, sizeof(datalog_day),
                  current_datalog_day_filter_);
        reset_run_locked(true);
        pending_run_kind_ = kind;
        copy_cstr(pending_datalog_day_, sizeof(pending_datalog_day_),
                  datalog_day);
        status_.state = SleepHqSyncState::Pending;
        status_.pending = true;
        copy_cstr(status_.pending_reason,
                  sizeof(status_.pending_reason),
                  reason);
        status_.last_error[0] = '\0';
        status_.updated_ms = nonzero_millis(millis());
        retry_due_ms_ = 0;
        retry_attempt_ = 0;
        Log::logf(CAT_SLEEPHQ, LOG_DEBUG,
                  "preempted; queued reason=%s\n",
                  status_.pending_reason);
        publish_runtime_locked();
        return;
    }

    copy_cstr(status_.last_error, sizeof(status_.last_error),
              error && *error ? error : "failed");
    status_.last_failure_epoch = storage_export_current_epoch_seconds_or_zero();
    status_.state = SleepHqSyncState::Error;
    status_.pending = false;
    status_.updated_ms = nonzero_millis(millis());
    status_.files_failed++;
    char retry_datalog_day[9] = {};
    copy_cstr(retry_datalog_day, sizeof(retry_datalog_day),
              current_datalog_day_filter_);
    const bool retryable =
        status_.configured && sleep_hq_error_retryable(status_.last_error);
    if (retryable) {
        const bool preserve_attempt =
            sleep_hq_error_preserves_retry_attempt(status_.last_error);
        const size_t backoff_count =
            sizeof(SLEEPHQ_RETRY_BACKOFF_MS) /
            sizeof(SLEEPHQ_RETRY_BACKOFF_MS[0]);
        const size_t index = preserve_attempt
            ? 0
            : (retry_attempt_ < backoff_count ? retry_attempt_
                                              : backoff_count - 1);
        retry_due_ms_ = status_.updated_ms + SLEEPHQ_RETRY_BACKOFF_MS[index];
        if (!preserve_attempt && retry_attempt_ < 255) retry_attempt_++;
    } else {
        retry_due_ms_ = 0;
        retry_attempt_ = 0;
    }
    const uint32_t retry_in_ms =
        retry_due_ms_ ? static_cast<uint32_t>(retry_due_ms_ -
                                              status_.updated_ms) : 0;
    Log::logf(CAT_SLEEPHQ, LOG_WARN,
              "failed phase=%u error=%s current=%s seen=%u uploaded=%u "
              "skipped=%u bytes=%llu retry_ms=%lu attempt=%u\n",
              static_cast<unsigned>(phase_),
              status_.last_error,
              current_file_.state().path[0]
                  ? current_file_.state().path
                  : "--",
              static_cast<unsigned>(status_.files_seen),
              static_cast<unsigned>(status_.files_uploaded),
              static_cast<unsigned>(status_.files_skipped),
              static_cast<unsigned long long>(status_.bytes_uploaded),
              static_cast<unsigned long>(retry_in_ms),
              static_cast<unsigned>(retry_attempt_));
    reset_run_locked(true);
    if (retryable && retry_datalog_day[0]) {
        copy_cstr(pending_datalog_day_, sizeof(pending_datalog_day_),
                  retry_datalog_day);
    }
    publish_runtime_locked();
}

void SleepHqSyncEngine::queue_retry_locked(uint32_t now_ms) {
    if (status_.state != SleepHqSyncState::Error ||
        retry_due_ms_ == 0 ||
        static_cast<int32_t>(now_ms - retry_due_ms_) < 0) {
        return;
    }
    status_.pending = true;
    status_.state = SleepHqSyncState::Pending;
    if (!status_.pending_reason[0]) {
        copy_cstr(status_.pending_reason,
                  sizeof(status_.pending_reason),
                  "retry");
    }
    status_.updated_ms = now_ms;
    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "retry queued reason=%s attempt=%u\n",
              status_.pending_reason,
              static_cast<unsigned>(retry_attempt_));
    publish_runtime_locked();
}

bool SleepHqSyncEngine::begin_export_planner_locked(char *error_out,
                                                 size_t error_out_size) {
    if (!export_inventory_) {
        copy_cstr(error_out, error_out_size, "export_inventory_missing");
        return false;
    }

    StorageExportPlannerConfig config;
    config.scope = StorageExportPlannerScope::SleepHq;
    config.state_dir = state_dir_;
    config.now_epoch = storage_export_current_epoch_seconds_or_zero();
    config.trust_completed_finalized_datalog_days = true;
    config.require_pending_datalog_file = true;
    config.defer_datalog_day_decision = true;
    if (current_run_kind_ == RunKind::PostTherapySync) {
        config.max_datalog_days = SLEEPHQ_POST_THERAPY_DATALOG_DAY_LIMIT;
    }
    if (current_datalog_day_filter_[0]) {
        config.only_datalog_day = current_datalog_day_filter_;
    }
    return export_planner_.begin(config,
                                 export_inventory_,
                                 error_out,
                                 error_out_size);
}

void SleepHqSyncEngine::reset_import_batch_locked() {
    if (!begin_inflight_remove_locked(InflightRemoveAction::ResetBatch)) {
        fail_locked("inflight_remove_prepare_failed");
    }
}

void SleepHqSyncEngine::complete_import_batch_reset_locked() {
    clear_current_file_locked();
    clear_staged_locked();
    state_batch_.clear();
    staged_validation_index_ = 0;
    status_.import_id = 0;
    status_.import_status[0] = '\0';
    import_process_started_ms_ = 0;
    import_poll_due_ms_ = 0;
    inflight_phase_ = InflightPhase::None;
    pending_inflight_phase_ = InflightPhase::None;
    pending_rebuild_day_[0] = '\0';
    pending_done_day_[0] = '\0';
    import_batch_active_ = false;
    phase_ = WorkPhase::NextFile;
}

ExportStep SleepHqSyncEngine::finish_import_or_sync_locked() {
    if (import_batch_active_) {
        reset_import_batch_locked();
        return phase_ == WorkPhase::RemoveInflight ? ExportStep::Waiting
                                                   : ExportStep::Idle;
    }
    finish_sync_locked();
    return ExportStep::Idle;
}

bool SleepHqSyncEngine::next_file_locked() {
    if (runtime_blocked_.load()) {
        fail_locked("preempted");
        return false;
    }
    if (!StorageService::status().available) {
        fail_locked("storage_unavailable");
        return false;
    }

    StorageExportPlannerItem item;
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    const StorageExportPlannerResult result =
        export_planner_.next(item, error, sizeof(error));
    switch (result) {
        case StorageExportPlannerResult::Item:
            if (item.kind ==
                StorageExportPlannerItemKind::DatalogDayComplete) {
                note_completed_datalog_day_locked(item.datalog_day);
                phase_ = staged_count_ == 0
                    ? pending_done_day_[0] ? WorkPhase::WriteDoneMarker
                                           : WorkPhase::NextFile
                    : WorkPhase::ProcessImport;
                if (staged_count_ != 0) import_batch_active_ = true;
                return true;
            }
            return plan_file_locked(item);
        case StorageExportPlannerResult::Yield:
            return true;
        case StorageExportPlannerResult::DecisionRequired: {
            bool local_complete = false;
            if (!export_planner_.pending_datalog_day_decision(
                    pending_remote_day_,
                    sizeof(pending_remote_day_),
                    local_complete)) {
                fail_locked("planner_decision_missing");
                return false;
            }
            pending_remote_day_local_complete_ = local_complete;

            bool needs_lookup = false;
            if (!resolve_pending_datalog_day_locked(needs_lookup,
                                                    error,
                                                    sizeof(error))) {
                fail_locked(error[0] ? error : "planner_decision_failed");
                return false;
            }
            if (needs_lookup) phase_ = WorkPhase::ResolveDatalogDay;
            return true;
        }
        case StorageExportPlannerResult::Done:
            phase_ = staged_count_ == 0 ? WorkPhase::Finish
                                        : WorkPhase::ProcessImport;
            if (staged_count_ != 0) import_batch_active_ = true;
            return true;
        case StorageExportPlannerResult::Error:
            fail_locked(error[0] ? error : "planner_failed");
            return false;
    }
    return true;
}

bool SleepHqSyncEngine::plan_file_locked(const StorageExportPlannerItem &item) {
    const char *path = item.path;
    clear_current_file_locked();
    copy_cstr(status_.current_path, sizeof(status_.current_path), path);
    status_.files_seen++;
    status_.updated_ms = nonzero_millis(millis());

    if (!item.info.exists || item.info.is_dir) {
        phase_ = WorkPhase::NextFile;
        return true;
    }

    char sleep_path[AC_STORAGE_PATH_MAX] = {};
    char name[AC_STORAGE_NAME_MAX] = {};
    if (!storage_export_build_relative_file_path(path,
                                                 sleep_path,
                                                 sizeof(sleep_path),
                                                 name,
                                                 sizeof(name))) {
        fail_locked("sleep_path_failed");
        return false;
    }
    if (!item.state_path[0]) {
        fail_locked("state_path_failed");
        return false;
    }

    const bool state_has_file = item.local_state_complete;
    if (!item.force_export && state_has_file) {
        status_.files_skipped++;
        clear_current_file_locked();
        phase_ = WorkPhase::NextFile;
        return true;
    }
    if (status_.import_id != 0 &&
        staged_contains_locked(path, item.info.size, item.info.mtime)) {
        status_.files_skipped++;
        clear_current_file_locked();
        phase_ = WorkPhase::NextFile;
        return true;
    }

    if (!stream_port_) {
        fail_locked("stream_port_unavailable");
        return false;
    }
    current_file_.configure(*stream_port_,
                            path,
                            sleep_path,
                            name,
                            item.state_path,
                            item.info.size,
                            item.info.mtime,
                            item.state_write_mode,
                            item.force_export && state_has_file);
    phase_ = status_.import_id == 0 ? WorkPhase::CreateImport
                                    : WorkPhase::OpenLocal;
    import_batch_active_ = true;
    return true;
}

bool SleepHqSyncEngine::build_inflight_path_locked(char *out,
                                                size_t out_size) const {
    if (!state_dir_[0] || !out || out_size == 0) return false;
    const int written = snprintf(out, out_size, "%s/%s",
                                 state_dir_, SLEEPHQ_INFLIGHT_FILE);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool SleepHqSyncEngine::staged_contains_locked(const char *path,
                                            uint64_t size,
                                            uint64_t mtime) const {
    if (!path) return false;
    for (size_t i = 0; i < staged_count_; ++i) {
        const StagedFile &entry = staged_[i];
        if (entry.size == size &&
            entry.mtime == mtime &&
            strcmp(entry.path, path) == 0) {
            return true;
        }
    }
    return false;
}

void SleepHqSyncEngine::note_completed_datalog_day_locked(const char *day) {
    pending_done_day_[0] = '\0';
    if (!day || !export_inventory_) return;
    if (!storage_export_datalog_day_finalized(
            day,
            export_inventory_->latest_datalog_day())) {
        return;
    }
    if (export_inventory_->datalog_day_done(day)) return;
    copy_cstr(pending_done_day_, sizeof(pending_done_day_), day);
}

uint32_t SleepHqSyncEngine::next_storage_generation_locked() {
    const uint32_t generation = next_storage_generation_;
    next_storage_generation_++;
    if (next_storage_generation_ == 0) next_storage_generation_ = 1;
    return generation;
}

bool SleepHqSyncEngine::build_inflight_bytes_locked(InflightPhase phase) {
    if (!status_.team_id || !status_.import_id ||
        phase == InflightPhase::None) {
        return false;
    }

    const char *rebuild_day = pending_rebuild_day_[0]
        ? pending_rebuild_day_
        : "-";
    const char *done_day = pending_done_day_[0] ? pending_done_day_ : "-";
    char header[128] = {};
    const int header_length = snprintf(
        header, sizeof(header), "v2\t%lu\t%lu\t%s\t%s\t%s\n",
        static_cast<unsigned long>(status_.team_id),
        static_cast<unsigned long>(status_.import_id),
        inflight_phase_name(phase), rebuild_day, done_day);
    if (header_length <= 0 ||
        static_cast<size_t>(header_length) >= sizeof(header)) {
        return false;
    }

    size_t total = static_cast<size_t>(header_length);
    char line[AC_STORAGE_PATH_MAX * 2 + AC_SLEEPHQ_CONTENT_HASH_MAX + 96] = {};
    for (size_t i = 0; i < staged_count_; ++i) {
        const StagedFile &entry = staged_[i];
        const char *hash = entry.content_hash[0] ? entry.content_hash : "-";
        const int line_length = snprintf(
            line, sizeof(line), "%llu\t%llu\t%s\t%lu\t%s\t%s\n",
            static_cast<unsigned long long>(entry.size),
            static_cast<unsigned long long>(entry.mtime), hash,
            static_cast<unsigned long>(entry.import_id), entry.path,
            entry.state_path);
        if (line_length <= 0 ||
            static_cast<size_t>(line_length) >= sizeof(line) ||
            static_cast<size_t>(line_length) > SIZE_MAX - total) {
            return false;
        }
        total += static_cast<size_t>(line_length);
    }
    if (total > SLEEPHQ_INFLIGHT_MAX_BYTES) return false;

    std::unique_ptr<LargeByteBuffer> bytes = LargeByteBuffer::allocate(total);
    if (!bytes) return false;

    size_t offset = 0;
    memcpy(bytes->data() + offset, header, static_cast<size_t>(header_length));
    offset += static_cast<size_t>(header_length);
    for (size_t i = 0; i < staged_count_; ++i) {
        const StagedFile &entry = staged_[i];
        const char *hash = entry.content_hash[0] ? entry.content_hash : "-";
        const int line_length = snprintf(
            line, sizeof(line), "%llu\t%llu\t%s\t%lu\t%s\t%s\n",
            static_cast<unsigned long long>(entry.size),
            static_cast<unsigned long long>(entry.mtime), hash,
            static_cast<unsigned long>(entry.import_id), entry.path,
            entry.state_path);
        if (line_length <= 0 ||
            static_cast<size_t>(line_length) >= sizeof(line)) {
            return false;
        }

        memcpy(bytes->data() + offset, line,
               static_cast<size_t>(line_length));
        offset += static_cast<size_t>(line_length);
    }
    if (offset != total) return false;

    pending_state_bytes_ = LargeByteBuffer::freeze(std::move(bytes));
    return pending_state_bytes_ != nullptr;
}

bool SleepHqSyncEngine::prepare_inflight_write_locked(
    InflightPhase phase,
    WorkPhase next_phase) {
    pending_state_path_[0] = '\0';
    pending_state_bytes_.reset();
    if (!build_inflight_path_locked(pending_state_path_,
                                    sizeof(pending_state_path_)) ||
        !build_inflight_bytes_locked(phase)) {
        return false;
    }

    pending_inflight_phase_ = phase;
    storage_next_phase_ = next_phase;
    phase_ = WorkPhase::WriteInflight;
    return true;
}

ExportStep SleepHqSyncEngine::step_write_inflight_locked() {
    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "inflight_write_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        state_io_.reset();
        inflight_phase_ = pending_inflight_phase_;
        pending_inflight_phase_ = InflightPhase::None;
        pending_state_path_[0] = '\0';
        pending_state_bytes_.reset();
        phase_ = storage_next_phase_;
        storage_next_phase_ = WorkPhase::Idle;
        return ExportStep::Working;
    }

    const OperationAdmission admission = state_io_.request_replace(
        pending_state_path_, pending_state_bytes_,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("inflight_write_rejected");
        return ExportStep::Idle;
    }
    return ExportStep::Waiting;
}

bool SleepHqSyncEngine::begin_inflight_remove_locked(
    InflightRemoveAction action,
    const char *failure) {
    pending_state_path_[0] = '\0';
    pending_state_bytes_.reset();
    state_io_.reset();
    if (action == InflightRemoveAction::None ||
        !build_inflight_path_locked(pending_state_path_,
                                    sizeof(pending_state_path_))) {
        return false;
    }

    inflight_remove_action_ = action;
    copy_cstr(storage_failure_, sizeof(storage_failure_),
              failure ? failure : "");
    phase_ = WorkPhase::RemoveInflight;
    return true;
}

ExportStep SleepHqSyncEngine::step_remove_inflight_locked() {
    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        const InflightRemoveAction action = inflight_remove_action_;
        char original_failure[AC_SLEEPHQ_ERROR_MAX] = {};
        copy_cstr(original_failure, sizeof(original_failure), storage_failure_);
        char remove_failure[AC_SLEEPHQ_ERROR_MAX] = {};
        copy_cstr(remove_failure, sizeof(remove_failure), state_io_.error());
        state_io_.reset();
        if (action == InflightRemoveAction::Fail && original_failure[0]) {
            Log::logf(CAT_SLEEPHQ, LOG_WARN,
                      "inflight cleanup failed error=%s\n",
                      remove_failure[0] ? remove_failure
                                        : "storage_remove_failed");
            fail_locked(original_failure);
        } else {
            fail_locked(remove_failure[0] ? remove_failure
                                          : "inflight_remove_failed");
        }
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        const InflightRemoveAction action = inflight_remove_action_;
        char failure[AC_SLEEPHQ_ERROR_MAX] = {};
        copy_cstr(failure, sizeof(failure), storage_failure_);
        state_io_.reset();
        pending_state_path_[0] = '\0';
        inflight_remove_action_ = InflightRemoveAction::None;
        storage_failure_[0] = '\0';
        inflight_phase_ = InflightPhase::None;

        switch (action) {
            case InflightRemoveAction::ResumeExport:
            case InflightRemoveAction::ResetBatch:
                complete_import_batch_reset_locked();
                return ExportStep::Working;
            case InflightRemoveAction::Fail:
                fail_locked(failure[0] ? failure : "failed");
                return ExportStep::Idle;
            case InflightRemoveAction::None:
                fail_locked("inflight_remove_action_missing");
                return ExportStep::Idle;
        }
    }

    const OperationAdmission admission = state_io_.request_remove(
        pending_state_path_, next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("inflight_remove_rejected");
        return ExportStep::Idle;
    }
    return ExportStep::Waiting;
}

void SleepHqSyncEngine::reset_inflight_reader_locked() {
    inflight_file_.reset();
    inflight_read_offset_ = 0;
    inflight_line_length_ = 0;
    inflight_header_seen_ = false;
    inflight_parse_failed_ = false;
    inflight_line_[0] = '\0';
}

bool SleepHqSyncEngine::parse_inflight_line_locked(char *line) {
    if (!line || !line[0]) return true;

    char *save = nullptr;
    if (!inflight_header_seen_) {
        char *version = strtok_r(line, "\t", &save);
        char *team_text = strtok_r(nullptr, "\t", &save);
        char *import_text = strtok_r(nullptr, "\t", &save);
        char *phase_text = strtok_r(nullptr, "\t", &save);
        char *rebuild_day = strtok_r(nullptr, "\t", &save);
        char *done_day = strtok_r(nullptr, "\t", &save);
        if (!version || !team_text || !import_text || !phase_text ||
            !rebuild_day || !done_day || strtok_r(nullptr, "\t", &save) ||
            strcmp(version, "v2") != 0) {
            return false;
        }

        uint32_t team_id = 0;
        uint32_t import_id = 0;
        InflightPhase phase = InflightPhase::None;
        if (!parse_uint32_text(team_text, team_id) || team_id == 0 ||
            !parse_uint32_text(import_text, import_id) || import_id == 0 ||
            !parse_inflight_phase(phase_text, phase) ||
            phase == InflightPhase::None ||
            (status_.team_id != 0 && status_.team_id != team_id)) {
            return false;
        }
        if ((strcmp(rebuild_day, "-") != 0 &&
             !storage_export_is_datalog_day_name(rebuild_day)) ||
            (strcmp(done_day, "-") != 0 &&
             !storage_export_is_datalog_day_name(done_day))) {
            return false;
        }

        status_.team_id = team_id;
        status_.import_id = import_id;
        inflight_phase_ = phase;
        copy_cstr(pending_rebuild_day_, sizeof(pending_rebuild_day_),
                  strcmp(rebuild_day, "-") == 0 ? "" : rebuild_day);
        copy_cstr(pending_done_day_, sizeof(pending_done_day_),
                  strcmp(done_day, "-") == 0 ? "" : done_day);
        inflight_header_seen_ = true;
        return true;
    }

    char *size_text = strtok_r(line, "\t", &save);
    char *mtime_text = strtok_r(nullptr, "\t", &save);
    char *hash = strtok_r(nullptr, "\t", &save);
    char *import_text = strtok_r(nullptr, "\t", &save);
    char *local_path = strtok_r(nullptr, "\t", &save);
    char *state_path = strtok_r(nullptr, "\t", &save);
    if (!size_text || !mtime_text || !hash || !import_text || !local_path ||
        !state_path || strtok_r(nullptr, "\t", &save)) {
        return false;
    }

    uint64_t size = 0;
    uint64_t mtime = 0;
    uint32_t import_id = 0;
    if (!parse_uint64_text(size_text, size) ||
        !parse_uint64_text(mtime_text, mtime) ||
        !parse_uint32_text(import_text, import_id) || import_id == 0 ||
        import_id != status_.import_id ||
        (strcmp(hash, "-") != 0 &&
         strlen(hash) >= AC_SLEEPHQ_CONTENT_HASH_MAX) ||
        !storage_user_path_valid(local_path) ||
        !storage_user_path_valid(state_path) ||
        !reserve_staged_locked(staged_count_ + 1)) {
        return false;
    }

    StagedFile &entry = staged_[staged_count_++];
    entry.size = size;
    entry.mtime = mtime;
    entry.import_id = import_id;
    copy_cstr(entry.content_hash, sizeof(entry.content_hash),
              strcmp(hash, "-") == 0 ? "" : hash);
    copy_cstr(entry.path, sizeof(entry.path), local_path);
    copy_cstr(entry.state_path, sizeof(entry.state_path), state_path);
    return true;
}

void SleepHqSyncEngine::complete_inflight_load_locked() {
    status_.files_uploaded = staged_count_;
    status_.bytes_uploaded = 0;
    for (size_t i = 0; i < staged_count_; ++i) {
        status_.bytes_uploaded += staged_[i].size;
    }

    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "resuming import=%lu phase=%s files=%u\n",
              static_cast<unsigned long>(status_.import_id),
              inflight_phase_name(inflight_phase_),
              static_cast<unsigned>(staged_count_));
    import_batch_active_ = true;
    if (inflight_phase_ == InflightPhase::Processing) {
        import_process_started_ms_ = nonzero_millis(millis());
        import_poll_due_ms_ = import_process_started_ms_;
        phase_ = WorkPhase::WaitImport;
    } else {
        phase_ = WorkPhase::NextFile;
    }
}

ExportStep SleepHqSyncEngine::step_load_inflight_locked() {
    if (inflight_file_.exists()) {
        uint8_t bytes[SLEEPHQ_INFLIGHT_READ_STEP_BYTES] = {};
        const size_t remaining = inflight_file_.size() - inflight_read_offset_;
        const size_t wanted = remaining < sizeof(bytes) ? remaining
                                                       : sizeof(bytes);
        const size_t read = inflight_file_.read(inflight_read_offset_,
                                                bytes, wanted);
        if (read != wanted) inflight_parse_failed_ = true;

        for (size_t i = 0; i < read && !inflight_parse_failed_; ++i) {
            const char ch = static_cast<char>(bytes[i]);
            if (ch == '\n') {
                inflight_line_[inflight_line_length_] = '\0';
                inflight_parse_failed_ =
                    !parse_inflight_line_locked(inflight_line_);
                inflight_line_length_ = 0;
            } else if (ch != '\r' &&
                       inflight_line_length_ + 1 < sizeof(inflight_line_)) {
                inflight_line_[inflight_line_length_++] = ch;
            } else if (ch != '\r') {
                inflight_parse_failed_ = true;
            }
        }
        inflight_read_offset_ += read;
        if (!inflight_parse_failed_ &&
            inflight_read_offset_ < inflight_file_.size()) {
            return ExportStep::Working;
        }
        if (!inflight_parse_failed_ && inflight_line_length_ != 0) {
            inflight_line_[inflight_line_length_] = '\0';
            inflight_parse_failed_ =
                !parse_inflight_line_locked(inflight_line_);
        }

        inflight_file_.reset();
        if (!inflight_parse_failed_ && inflight_header_seen_) {
            complete_inflight_load_locked();
            return phase_ == WorkPhase::WaitImport ? ExportStep::Waiting
                                                   : ExportStep::Working;
        }

        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "discarded incompatible inflight state\n");
        clear_staged_locked();
        status_.import_id = 0;
        pending_rebuild_day_[0] = '\0';
        pending_done_day_[0] = '\0';
        if (!begin_inflight_remove_locked(
                InflightRemoveAction::ResumeExport)) {
            fail_locked("inflight_remove_prepare_failed");
            return ExportStep::Idle;
        }
        return ExportStep::Waiting;
    }

    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "inflight_read_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        inflight_file_ = state_io_.take_file();
        if (!inflight_file_.exists()) {
            reset_inflight_reader_locked();
            phase_ = WorkPhase::NextFile;
            return ExportStep::Working;
        }
        if (inflight_file_.size() == 0) {
            inflight_parse_failed_ = true;
        }
        return ExportStep::Working;
    }

    if (!build_inflight_path_locked(pending_state_path_,
                                    sizeof(pending_state_path_))) {
        fail_locked("inflight_path_failed");
        return ExportStep::Idle;
    }
    const OperationAdmission admission = state_io_.request_read(
        pending_state_path_, SLEEPHQ_INFLIGHT_MAX_BYTES,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("inflight_read_rejected");
        return ExportStep::Idle;
    }
    return ExportStep::Waiting;
}

bool SleepHqSyncEngine::prepare_staged_state_locked() {
    state_batch_.clear();
    for (size_t i = 0; i < staged_count_; ++i) {
        const StagedFile &entry = staged_[i];
        if (!state_batch_.add(entry.state_path, entry.path,
                              entry.size, entry.mtime)) {
            state_batch_.clear();
            return false;
        }
    }
    return !state_batch_.empty();
}

ExportStep SleepHqSyncEngine::step_validate_staged_locked() {
    if (staged_validation_index_ >= staged_count_) {
        if (!prepare_staged_state_locked()) {
            fail_locked("state_batch_failed");
            return ExportStep::Idle;
        }
        phase_ = WorkPhase::FlushState;
        return ExportStep::Working;
    }

    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "staged_stat_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        const StorageFileInfo info = state_io_.info();
        state_io_.reset();
        const StagedFile &entry = staged_[staged_validation_index_];
        if (!info.exists || info.directory || info.size != entry.size ||
            info.modified != entry.mtime) {
            if (!begin_inflight_remove_locked(
                    InflightRemoveAction::Fail,
                    "local_changed_after_upload")) {
                fail_locked("inflight_remove_prepare_failed");
                return ExportStep::Idle;
            }
            return ExportStep::Waiting;
        }

        staged_validation_index_++;
        return ExportStep::Working;
    }

    const StagedFile &entry = staged_[staged_validation_index_];
    const OperationAdmission admission = state_io_.request_stat(
        entry.path, next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("staged_stat_rejected");
        return ExportStep::Idle;
    }
    return ExportStep::Waiting;
}

void SleepHqSyncEngine::continue_after_state_flush_locked() {
    if (pending_rebuild_day_[0]) {
        phase_ = WorkPhase::WriteRebuildMarker;
        return;
    }
    if (pending_done_day_[0]) {
        phase_ = WorkPhase::WriteDoneMarker;
        return;
    }
    if (!begin_inflight_remove_locked(InflightRemoveAction::ResetBatch)) {
        fail_locked("inflight_remove_prepare_failed");
    }
}

ExportStep SleepHqSyncEngine::step_flush_state_locked() {
    if (state_batch_.empty()) {
        continue_after_state_flush_locked();
        return phase_ == WorkPhase::RemoveInflight ? ExportStep::Waiting
                                                   : ExportStep::Working;
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
        if (state_batch_.empty()) continue_after_state_flush_locked();
        return phase_ == WorkPhase::RemoveInflight ? ExportStep::Waiting
                                                   : ExportStep::Working;
    }

    if (!export_inventory_) {
        fail_locked("export_inventory_missing");
        return ExportStep::Idle;
    }
    const char *state_path = state_batch_.first_state_path();
    if (!state_path || !state_path[0]) {
        fail_locked("state_path_missing");
        return ExportStep::Idle;
    }
    copy_cstr(pending_state_path_, sizeof(pending_state_path_), state_path);
    pending_state_bytes_ = state_batch_.build_file(*export_inventory_,
                                                   state_path);
    if (!pending_state_bytes_) {
        fail_locked("state_file_build_failed");
        return ExportStep::Idle;
    }

    const OperationAdmission admission = state_io_.request_replace(
        pending_state_path_, pending_state_bytes_,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("state_write_rejected");
        return ExportStep::Idle;
    }
    return ExportStep::Waiting;
}

bool SleepHqSyncEngine::prepare_rebuild_marker_locked() {
    const uint64_t now_epoch = storage_export_current_epoch_seconds_or_zero();
    if (!pending_rebuild_day_[0] || now_epoch == 0 ||
        !build_datalog_rebuild_marker_path_locked(
            pending_rebuild_day_, pending_state_path_,
            sizeof(pending_state_path_))) {
        return false;
    }

    char marker[32] = {};
    const int length = snprintf(marker, sizeof(marker), "%llu\n",
                                static_cast<unsigned long long>(now_epoch));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(marker)) {
        return false;
    }
    pending_state_bytes_ = freeze_bytes(marker, static_cast<size_t>(length));
    return pending_state_bytes_ != nullptr;
}

ExportStep SleepHqSyncEngine::step_write_rebuild_marker_locked() {
    if (!pending_rebuild_day_[0]) {
        phase_ = pending_done_day_[0] ? WorkPhase::WriteDoneMarker
                                      : WorkPhase::RemoveInflight;
        if (phase_ == WorkPhase::RemoveInflight &&
            !begin_inflight_remove_locked(InflightRemoveAction::ResetBatch)) {
            fail_locked("inflight_remove_prepare_failed");
            return ExportStep::Idle;
        }
        return phase_ == WorkPhase::RemoveInflight ? ExportStep::Waiting
                                                   : ExportStep::Working;
    }

    const StorageFileClientResult io_result = state_io_.poll();
    if (io_result == StorageFileClientResult::Waiting) {
        return ExportStep::Waiting;
    }
    if (io_result == StorageFileClientResult::Error) {
        fail_locked(state_io_.error()[0] ? state_io_.error()
                                         : "rebuild_marker_write_failed");
        return ExportStep::Idle;
    }
    if (io_result == StorageFileClientResult::Ready) {
        state_io_.reset();
        pending_rebuild_day_[0] = '\0';
        pending_state_path_[0] = '\0';
        pending_state_bytes_.reset();
        phase_ = pending_done_day_[0] ? WorkPhase::WriteDoneMarker
                                      : WorkPhase::RemoveInflight;
        if (phase_ == WorkPhase::RemoveInflight &&
            !begin_inflight_remove_locked(InflightRemoveAction::ResetBatch)) {
            fail_locked("inflight_remove_prepare_failed");
            return ExportStep::Idle;
        }
        return phase_ == WorkPhase::RemoveInflight ? ExportStep::Waiting
                                                   : ExportStep::Working;
    }

    if (!prepare_rebuild_marker_locked()) {
        fail_locked("rebuild_marker_build_failed");
        return ExportStep::Idle;
    }
    const OperationAdmission admission = state_io_.request_replace(
        pending_state_path_, pending_state_bytes_,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("rebuild_marker_write_rejected");
        return ExportStep::Idle;
    }
    return ExportStep::Waiting;
}

bool SleepHqSyncEngine::prepare_done_marker_locked() {
    if (!pending_done_day_[0] ||
        !storage_export_build_done_path(state_dir_, pending_done_day_,
                                        pending_state_path_,
                                        sizeof(pending_state_path_))) {
        return false;
    }

    static constexpr char DONE_MARKER[] = "done\n";
    pending_state_bytes_ = freeze_bytes(DONE_MARKER,
                                        sizeof(DONE_MARKER) - 1);
    return pending_state_bytes_ != nullptr;
}

ExportStep SleepHqSyncEngine::step_write_done_marker_locked() {
    if (!pending_done_day_[0]) {
        if (!begin_inflight_remove_locked(InflightRemoveAction::ResetBatch)) {
            fail_locked("inflight_remove_prepare_failed");
            return ExportStep::Idle;
        }
        return ExportStep::Waiting;
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
        Log::logf(CAT_SLEEPHQ, LOG_INFO,
                  "DATALOG day complete day=%s\n", pending_done_day_);
        pending_done_day_[0] = '\0';
        pending_state_path_[0] = '\0';
        pending_state_bytes_.reset();
        if (!begin_inflight_remove_locked(InflightRemoveAction::ResetBatch)) {
            fail_locked("inflight_remove_prepare_failed");
            return ExportStep::Idle;
        }
        return ExportStep::Waiting;
    }

    if (!prepare_done_marker_locked()) {
        fail_locked("done_marker_build_failed");
        return ExportStep::Idle;
    }
    const OperationAdmission admission = state_io_.request_replace(
        pending_state_path_, pending_state_bytes_,
        next_storage_generation_locked());
    if (admission == OperationAdmission::Busy) return ExportStep::Waiting;
    if (admission != OperationAdmission::Accepted) {
        fail_locked("done_marker_write_rejected");
        return ExportStep::Idle;
    }
    return ExportStep::Waiting;
}

bool SleepHqSyncEngine::reserve_staged_locked(size_t needed) {
    if (needed <= staged_capacity_) return true;
    size_t next = staged_capacity_ == 0 ? 8 : staged_capacity_ * 2;
    while (next < needed) next *= 2;
    StagedFile *items = static_cast<StagedFile *>(
        Memory::alloc_large(sizeof(StagedFile) * next, false));
    if (!items) {
        Log::logf(CAT_SLEEPHQ, LOG_ERROR,
                  "staged allocation failed entries=%u bytes=%u\n",
                  static_cast<unsigned>(next),
                  static_cast<unsigned>(sizeof(StagedFile) * next));
        return false;
    }
    for (size_t i = 0; i < next; ++i) new (&items[i]) StagedFile();
    for (size_t i = 0; i < staged_count_; ++i) items[i] = staged_[i];
    if (staged_) Memory::free(staged_);
    staged_ = items;
    staged_capacity_ = next;
    return true;
}

bool SleepHqSyncEngine::add_staged_locked(const SleepHqUploadResult &upload) {
    if (!reserve_staged_locked(staged_count_ + 1)) return false;

    const SleepHqSyncFileState &file = current_file_.state();
    StagedFile &entry = staged_[staged_count_++];
    copy_cstr(entry.path, sizeof(entry.path), file.path);
    copy_cstr(entry.state_path, sizeof(entry.state_path), file.state_path);
    copy_cstr(entry.content_hash, sizeof(entry.content_hash),
              upload.content_hash);
    entry.size = file.size;
    entry.mtime = file.mtime;
    entry.import_id = status_.import_id;
    return true;
}

bool SleepHqSyncEngine::remote_file_cache_contains_locked() const {
    const SleepHqSyncFileState &file = current_file_.state();
    if (!file.name[0] || !file.sleep_path[0] || !file.content_hash[0]) {
        return false;
    }
    return remote_files_.contains(file.name, file.sleep_path,
                                  file.content_hash, file.size);
}

bool SleepHqSyncEngine::remote_file_list_cb(void *ctx,
                                         const SleepHqRemoteFile &file) {
    SleepHqRemoteFileCache *cache =
        static_cast<SleepHqRemoteFileCache *>(ctx);
    return cache && cache->add(file);
}

bool SleepHqSyncEngine::remote_machine_list_cb(void *ctx,
                                            const SleepHqMachine &machine) {
    MachineListContext *lookup = static_cast<MachineListContext *>(ctx);
    if (!lookup || lookup->machine_id || !lookup->serial) return lookup != nullptr;
    if (machine.id && strcmp(machine.serial_number, lookup->serial) == 0) {
        lookup->machine_id = machine.id;
    }
    return true;
}

bool SleepHqSyncEngine::read_local_machine_serial(
    char *out,
    size_t out_size,
    const BackgroundOperationControl &operation,
    char *error,
    size_t error_size) {
    if (!out || out_size == 0) {
        copy_cstr(error, error_size, "bad_serial_buffer");
        return false;
    }
    out[0] = '\0';
    if (!export_inventory_ || !stream_port_) {
        copy_cstr(error, error_size, "identification_inventory_missing");
        return false;
    }

    StorageExportInventoryEntryView entry;
    if (!export_inventory_->find_file(SLEEPHQ_IDENTIFICATION_PATH, entry)) {
        return true;
    }
    if (!entry.info.exists || entry.info.is_dir || entry.info.size == 0 ||
        entry.info.size > SLEEPHQ_IDENTIFICATION_JSON_MAX) {
        copy_cstr(error, error_size, "identification_invalid");
        return false;
    }

    const size_t size = static_cast<size_t>(entry.info.size);
    char *json = static_cast<char *>(Memory::alloc_large(size + 1, false));
    if (!json) {
        copy_cstr(error, error_size, "identification_alloc");
        return false;
    }

    StorageStreamReader reader;
    reader.configure(*stream_port_, SLEEPHQ_IDENTIFICATION_PATH,
                     entry.info.size, entry.info.mtime);
    const bool ok = reader.open(operation, error, error_size) &&
        reader.read_exact(reinterpret_cast<uint8_t *>(json), size,
                          operation, error, error_size);
    reader.close(ok);
    if (!ok) {
        Memory::free(json);
        if (!error[0]) {
            copy_cstr(error, error_size, "identification_read");
        }
        return false;
    }
    json[size] = '\0';

    JsonDocument doc;
    DeserializationError parse_error = deserializeJson(doc, json);
    Memory::free(json);
    if (parse_error) {
        copy_cstr(error, error_size, "identification_json");
        return false;
    }

    const char *serial = json_serial_or_empty(doc);
    if (serial && serial[0]) {
        copy_cstr(out, out_size, serial);
    }
    return true;
}

void SleepHqSyncEngine::note_remote_machine_missing_locked() {
    if (remote_reconcile_all_missing_) return;
    remote_reconcile_all_missing_ = true;
    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "remote machine missing serial=%s; pending DATALOG days will rebuild\n",
              remote_serial_);
}

bool SleepHqSyncEngine::prepare_remote_reconcile_locked(char *error,
                                                     size_t error_size) {
    remote_machine_id_ = 0;
    remote_machine_next_page_ = 1;
    remote_machine_pages_loaded_ = 0;
    remote_reconcile_enabled_ = false;
    remote_reconcile_all_missing_ = false;
    remote_serial_[0] = '\0';
    remote_date_count_ = 0;

    copy_cstr(error, error_size, "");
    return true;
}

bool SleepHqSyncEngine::resolve_pending_datalog_day_locked(
    bool &needs_lookup,
    char *error,
    size_t error_size) {
    needs_lookup = false;
    if (!pending_remote_day_[0]) {
        copy_cstr(error, error_size, "bad_datalog_day");
        return false;
    }

    if (!remote_reconcile_enabled_) {
        const bool resolved = export_planner_.resolve_datalog_day_decision(
            false, error, error_size);
        if (resolved) {
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
            phase_ = WorkPhase::NextFile;
        }
        return resolved;
    }
    if (remote_reconcile_all_missing_) {
        return begin_remote_missing_datalog_day_locked(error, error_size);
    }

    bool exists = false;
    if (cached_remote_date_exists_locked(pending_remote_day_, exists)) {
        if (!exists) {
            return begin_remote_missing_datalog_day_locked(error, error_size);
        }
        const bool resolved = export_planner_.resolve_datalog_day_decision(
            false, error, error_size);
        if (resolved) {
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
            phase_ = WorkPhase::NextFile;
        }
        return resolved;
    }

    char iso_date[11] = {};
    if (!datalog_day_to_iso_date(pending_remote_day_,
                                 iso_date,
                                 sizeof(iso_date))) {
        copy_cstr(error, error_size, "bad_datalog_day");
        return false;
    }
    needs_lookup = true;
    return true;
}

ExportStep SleepHqSyncEngine::begin_export_work_locked() {
    char planner_error[AC_SLEEPHQ_ERROR_MAX] = {};
    if (!begin_export_planner_locked(planner_error, sizeof(planner_error))) {
        fail_locked(planner_error[0] ? planner_error : "planner_failed");
        return ExportStep::Idle;
    }
    clear_staged_locked();
    status_.import_id = 0;
    inflight_phase_ = InflightPhase::None;
    reset_inflight_reader_locked();
    state_io_.reset();
    phase_ = WorkPhase::LoadInflight;
    return ExportStep::Working;
}

ExportStep SleepHqSyncEngine::step_resolve_remote_file_locked() {
    const SleepHqSyncFileState &file = current_file_.state();
    if (!current_file_.has_content_hash()) {
        phase_ = WorkPhase::HashLocalFile;
        return ExportStep::Working;
    }

    if (file.attach_by_hash || remote_file_cache_contains_locked()) {
        current_file_.set_attach_by_hash(true);
        phase_ = WorkPhase::UploadFile;
        return ExportStep::Working;
    }

    if (!remote_file_cache_complete_ &&
        remote_file_pages_loaded_ < SLEEPHQ_REMOTE_FILE_PAGE_LIMIT) {
        phase_ = WorkPhase::FetchRemoteFiles;
        return ExportStep::Working;
    }

    remote_file_cache_complete_ = true;
    phase_ = WorkPhase::UploadFile;
    return ExportStep::Working;
}

bool SleepHqSyncEngine::operation_abort_cb(void *ctx) {
    SleepHqSyncEngine *job = static_cast<SleepHqSyncEngine *>(ctx);
    return !job || job->abort_requested_.load() ||
           job->runtime_blocked_.load();
}

BackgroundOperationControl SleepHqSyncEngine::operation_control(
    uint32_t timeout_ms) const {
    BackgroundOperationControl operation;
    operation.started_ms = millis();
    operation.timeout_ms = timeout_ms;
    operation.should_abort = &SleepHqSyncEngine::operation_abort_cb;
    operation.ctx = const_cast<SleepHqSyncEngine *>(this);
    return operation;
}

void SleepHqSyncEngine::request_operation_abort() {
    bool expected = false;
    if (abort_requested_.compare_exchange_strong(expected, true)) {
        operation_generation_.fetch_add(1);
    }
}

ExportStep SleepHqSyncEngine::step_wait_import_locked() {
    if (staged_count_ == 0) {
        phase_ = WorkPhase::Finish;
        return ExportStep::Working;
    }

    const uint32_t now = nonzero_millis(millis());
    if (import_poll_due_ms_ != 0 &&
        static_cast<int32_t>(now - import_poll_due_ms_) < 0) {
        return ExportStep::Waiting;
    }
    phase_ = WorkPhase::FetchImport;
    return ExportStep::Working;
}

bool SleepHqSyncEngine::phase_has_blocking_io(WorkPhase phase) {
    switch (phase) {
        case WorkPhase::Connect:
        case WorkPhase::ReadIdentification:
        case WorkPhase::FindRemoteMachine:
        case WorkPhase::ResolveDatalogDay:
        case WorkPhase::CreateImport:
        case WorkPhase::OpenLocal:
        case WorkPhase::HashLocalFile:
        case WorkPhase::FetchRemoteFiles:
        case WorkPhase::UploadFile:
        case WorkPhase::ProcessImport:
        case WorkPhase::FetchImport:
            return true;

        case WorkPhase::Idle:
        case WorkPhase::LoadInventory:
        case WorkPhase::LoadInflight:
        case WorkPhase::NextFile:
        case WorkPhase::ReadRebuildMarker:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::WaitImport:
        case WorkPhase::WriteInflight:
        case WorkPhase::ValidateStaged:
        case WorkPhase::FlushState:
        case WorkPhase::WriteRebuildMarker:
        case WorkPhase::WriteDoneMarker:
        case WorkPhase::RemoveInflight:
        case WorkPhase::Finish:
            return false;
    }
    return false;
}

void SleepHqSyncEngine::execute_blocking_phase(WorkPhase phase,
                                            BlockingResult &result) {
    BackgroundOperationControl operation =
        operation_control(SLEEPHQ_API_OPERATION_TIMEOUT_MS);

    switch (phase) {
        case WorkPhase::ReadIdentification:
            result.ok = read_local_machine_serial(
                result.serial,
                sizeof(result.serial),
                operation,
                result.error,
                sizeof(result.error));
            return;

        case WorkPhase::Connect: {
            if (!client_.configure(client_config_from_snapshot(config_))) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          "not_configured");
                return;
            }

            result.team_id =
                current_run_kind_ == RunKind::Check ? 0 : status_.team_id;
            if (result.team_id == 0 &&
                !client_.resolve_team_id(result.team_id, &operation)) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          client_.last_error());
                return;
            }
            result.ok = result.team_id != 0;
            if (!result.ok) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          "team_id_missing");
            }
            return;
        }

        case WorkPhase::FindRemoteMachine: {
            if (!remote_reconcile_enabled_ || !remote_serial_[0] ||
                remote_machine_id_ || remote_reconcile_all_missing_ ||
                remote_machine_pages_loaded_ >=
                    SLEEPHQ_REMOTE_MACHINE_PAGE_LIMIT) {
                result.ok = true;
                return;
            }

            MachineListContext lookup;
            lookup.serial = remote_serial_;
            result.performed = true;
            result.ok = client_.list_team_machines(
                status_.team_id,
                remote_machine_next_page_,
                SLEEPHQ_REMOTE_MACHINE_PER_PAGE,
                &SleepHqSyncEngine::remote_machine_list_cb,
                &lookup,
                result.count,
                result.has_more,
                &operation);
            result.machine_id = lookup.machine_id;
            if (!result.ok) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          client_.last_error());
                result.retryable = sleep_hq_error_retryable(result.error);
            }
            return;
        }

        case WorkPhase::ResolveDatalogDay: {
            char iso_date[11] = {};
            if (!datalog_day_to_iso_date(pending_remote_day_,
                                         iso_date,
                                         sizeof(iso_date))) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          "bad_datalog_day");
                return;
            }

            SleepHqMachineDate remote_date;
            result.performed = true;
            if (client_.get_machine_date(remote_machine_id_,
                                         iso_date,
                                         remote_date,
                                         &operation)) {
                result.ok = true;
                result.remote_date_exists = true;
                return;
            }

            copy_cstr(result.error,
                      sizeof(result.error),
                      client_.last_error());
            if (strcmp(result.error, "http_404") == 0) {
                result.ok = true;
                return;
            }
            result.retryable = sleep_hq_error_retryable(result.error);
            return;
        }

        case WorkPhase::CreateImport:
            result.performed = true;
            result.ok = client_.create_import(status_.team_id,
                                               result.import,
                                               &operation);
            if (!result.ok) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          client_.last_error());
            }
            return;

        case WorkPhase::OpenLocal: {
            result.ok = current_file_.open(operation,
                                           result.error,
                                           sizeof(result.error));
            if (!result.ok) {
                if (!result.error[0]) {
                    copy_cstr(result.error,
                              sizeof(result.error),
                              "local_open_failed");
                }
            }
            return;
        }

        case WorkPhase::HashLocalFile:
            result.ok = current_file_.compute_content_hash(
                result.content_hash,
                sizeof(result.content_hash),
                operation,
                result.error,
                sizeof(result.error));
            if (!result.ok) {
                if (!result.error[0]) {
                    copy_cstr(result.error,
                              sizeof(result.error),
                              "hash_failed");
                }
            }
            return;

        case WorkPhase::FetchRemoteFiles:
            result.performed = true;
            result.ok = client_.list_team_files(
                status_.team_id,
                remote_file_next_page_,
                SLEEPHQ_REMOTE_FILE_PER_PAGE,
                &SleepHqSyncEngine::remote_file_list_cb,
                &result.remote_files,
                result.count,
                result.has_more,
                &operation);
            if (!result.ok) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          client_.last_error());
            }
            return;

        case WorkPhase::UploadFile: {
            const SleepHqSyncFileState &file = current_file_.state();
            SleepHqSyncFile::UploadReader upload_reader(current_file_,
                                                         operation);
            bool attached = false;
            bool stream_ready = false;
            operation.timeout_ms = sleep_hq_upload_timeout_ms(file.size);
            operation.started_ms = millis();

            if (file.attach_by_hash) {
                SleepHqAttachRequest attach;
                attach.import_id = status_.import_id;
                attach.name = file.name;
                attach.path = file.sleep_path;
                attach.content_hash = file.content_hash;
                if (client_.attach_file(attach, result.upload, &operation)) {
                    attached = true;
                }
            }

            if (!attached) {
                stream_ready = upload_reader.open();
                if (!stream_ready) {
                    copy_cstr(result.error,
                              sizeof(result.error),
                              "local_stream_open_failed");
                    return;
                }

                SleepHqUploadRequest request;
                request.import_id = status_.import_id;
                request.name = file.name;
                request.path = file.sleep_path;
                request.content_hash = file.content_hash[0]
                    ? file.content_hash
                    : nullptr;
                request.size = file.size;
                request.read = &SleepHqSyncFile::UploadReader::read_callback;
                request.reset = &SleepHqSyncFile::UploadReader::reset_callback;
                request.ctx = &upload_reader;
                request.operation = &operation;
                if (!client_.upload_file(request, result.upload)) {
                    copy_cstr(result.error,
                              sizeof(result.error),
                              client_.last_error());
                    return;
                }
            }

            result.ok = true;
            return;
        }

        case WorkPhase::ProcessImport:
            result.performed = true;
            result.ok = client_.process_import(status_.import_id,
                                               nullptr,
                                               &operation);
            if (!result.ok) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          client_.last_error());
            }
            return;

        case WorkPhase::FetchImport:
            result.performed = true;
            result.ok = client_.get_import(status_.import_id,
                                           result.import,
                                           &operation);
            if (!result.ok) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          client_.last_error());
            }
            return;

        case WorkPhase::Idle:
        case WorkPhase::LoadInventory:
        case WorkPhase::LoadInflight:
        case WorkPhase::NextFile:
        case WorkPhase::ReadRebuildMarker:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::WaitImport:
        case WorkPhase::WriteInflight:
        case WorkPhase::ValidateStaged:
        case WorkPhase::FlushState:
        case WorkPhase::WriteRebuildMarker:
        case WorkPhase::WriteDoneMarker:
        case WorkPhase::RemoveInflight:
        case WorkPhase::Finish:
            copy_cstr(result.error,
                      sizeof(result.error),
                      "invalid_blocking_phase");
            return;
    }
}

ExportStep SleepHqSyncEngine::publish_blocking_phase_locked(
    WorkPhase phase,
    BlockingResult &result) {
    if (status_.state != SleepHqSyncState::Working || phase_ != phase) {
        current_file_.close(false);
        client_.disconnect();
        return ExportStep::Idle;
    }

    if (!background_operation_result_current(
            result.operation_generation,
            operation_generation_.load(),
            abort_requested_.load()) ||
        pending_config_valid_ ||
        runtime_blocked_.load() ||
        !network_available_.load()) {
        current_file_.close(false);
        fail_locked("preempted");
        apply_pending_config_locked();
        return status_.pending ? ExportStep::Waiting : ExportStep::Idle;
    }

    if (!result.ok) {
        current_file_.close(false);
        if (phase == WorkPhase::ReadIdentification) {
            Log::logf(CAT_SLEEPHQ,
                      LOG_WARN,
                      "remote reconcile disabled: %s\n",
                      result.error[0]
                          ? result.error
                          : "identification_failed");
            remote_reconcile_enabled_ = false;
            remote_serial_[0] = '\0';
            phase_ = WorkPhase::FindRemoteMachine;
            return ExportStep::Working;
        }
        if (phase == WorkPhase::FindRemoteMachine && result.retryable) {
            Log::logf(CAT_SLEEPHQ,
                      LOG_WARN,
                      "remote machine lookup skipped: %s\n",
                      result.error[0] ? result.error : "machine_list_failed");
            remote_reconcile_enabled_ = false;
            return begin_export_work_locked();
        }

        if (phase == WorkPhase::ResolveDatalogDay && result.retryable) {
            Log::logf(CAT_SLEEPHQ,
                      LOG_DEBUG,
                      "remote machine-date lookup skipped day=%s error=%s\n",
                      pending_remote_day_,
                      result.error[0]
                          ? result.error
                          : "machine_date_lookup_failed");
            char planner_error[AC_SLEEPHQ_ERROR_MAX] = {};
            if (!export_planner_.resolve_datalog_day_decision(
                    false,
                    planner_error,
                    sizeof(planner_error))) {
                fail_locked(planner_error[0]
                                ? planner_error
                                : "planner_decision_failed");
                return ExportStep::Idle;
            }
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
            phase_ = WorkPhase::NextFile;
            return ExportStep::Working;
        }

        if (phase == WorkPhase::FetchRemoteFiles) {
            Log::logf(CAT_SLEEPHQ,
                      LOG_WARN,
                      "remote file lookup failed; falling back to upload: %s\n",
                      result.error[0]
                          ? result.error
                          : "remote_file_list_failed");
            remote_file_cache_complete_ = true;
            phase_ = WorkPhase::UploadFile;
            return ExportStep::Working;
        }

        if (phase == WorkPhase::FetchImport &&
            strcmp(result.error, "http_404") == 0) {
            if (!begin_inflight_remove_locked(
                    InflightRemoveAction::Fail,
                    result.error[0] ? result.error : "http_404")) {
                fail_locked("inflight_remove_prepare_failed");
                return ExportStep::Idle;
            }
            return ExportStep::Waiting;
        }
        fail_locked(result.error[0] ? result.error : "network_io_failed");
        return ExportStep::Idle;
    }

    switch (phase) {
        case WorkPhase::ReadIdentification:
            if (!result.serial[0]) {
                Log::logf(
                    CAT_SLEEPHQ,
                    LOG_WARN,
                    "remote reconcile disabled: identification serial missing\n");
                remote_reconcile_enabled_ = false;
            } else {
                copy_cstr(remote_serial_, sizeof(remote_serial_),
                          result.serial);
                remote_reconcile_enabled_ = true;
            }
            phase_ = WorkPhase::FindRemoteMachine;
            return ExportStep::Working;

        case WorkPhase::Connect: {
            status_.team_id = result.team_id;
            if (current_run_kind_ == RunKind::Check) {
                finish_check_locked(result.team_id);
                return ExportStep::Idle;
            }
            if (!build_endpoint_state_dir_locked(result.team_id,
                                                 state_dir_,
                                                 sizeof(state_dir_))) {
                fail_locked("state_path_failed");
                return ExportStep::Idle;
            }

            phase_ = WorkPhase::LoadInventory;
            return ExportStep::Working;
        }

        case WorkPhase::FindRemoteMachine:
            if (!result.performed) {
                if (remote_reconcile_enabled_ && remote_serial_[0] &&
                    !remote_machine_id_ && !remote_reconcile_all_missing_ &&
                    remote_machine_pages_loaded_ >=
                        SLEEPHQ_REMOTE_MACHINE_PAGE_LIMIT) {
                    note_remote_machine_missing_locked();
                }
                return begin_export_work_locked();
            }

            remote_machine_pages_loaded_++;
            remote_machine_next_page_++;
            if (result.machine_id) {
                remote_machine_id_ = result.machine_id;
                Log::logf(CAT_SLEEPHQ,
                          LOG_DEBUG,
                          "remote machine matched serial=%s id=%lu\n",
                          remote_serial_,
                          static_cast<unsigned long>(remote_machine_id_));
                return begin_export_work_locked();
            }
            if (!result.has_more ||
                remote_machine_pages_loaded_ >=
                    SLEEPHQ_REMOTE_MACHINE_PAGE_LIMIT) {
                note_remote_machine_missing_locked();
                return begin_export_work_locked();
            }
            return ExportStep::Working;

        case WorkPhase::ResolveDatalogDay: {
            if (!cache_remote_date_locked(pending_remote_day_,
                                          result.remote_date_exists)) {
                fail_locked("remote_date_cache_alloc");
                return ExportStep::Idle;
            }

            char error[AC_SLEEPHQ_ERROR_MAX] = {};
            if (!result.remote_date_exists) {
                if (!begin_remote_missing_datalog_day_locked(
                        error, sizeof(error))) {
                    fail_locked(error[0] ? error
                                         : "remote_date_decision_failed");
                    return ExportStep::Idle;
                }
                return phase_ == WorkPhase::ReadRebuildMarker
                    ? ExportStep::Waiting
                    : ExportStep::Working;
            }
            if (!export_planner_.resolve_datalog_day_decision(
                    false, error, sizeof(error))) {
                fail_locked(error[0] ? error : "planner_decision_failed");
                return ExportStep::Idle;
            }
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
            phase_ = WorkPhase::NextFile;
            return ExportStep::Working;
        }

        case WorkPhase::CreateImport:
            if (!result.import.id) {
                fail_locked("import_id_missing");
                return ExportStep::Idle;
            }
            status_.import_id = result.import.id;
            if (!prepare_inflight_write_locked(InflightPhase::Uploading,
                                               WorkPhase::OpenLocal)) {
                fail_locked("inflight_write_failed");
                return ExportStep::Idle;
            }
            return ExportStep::Working;

        case WorkPhase::OpenLocal:
            phase_ = WorkPhase::ResolveRemoteFile;
            return ExportStep::Working;

        case WorkPhase::HashLocalFile:
            current_file_.set_content_hash(result.content_hash);
            phase_ = WorkPhase::ResolveRemoteFile;
            return ExportStep::Working;

        case WorkPhase::FetchRemoteFiles:
            if (!remote_files_.merge_from(result.remote_files)) {
                fail_locked("remote_file_cache_alloc");
                return ExportStep::Idle;
            }
            remote_file_pages_loaded_++;
            remote_file_next_page_++;
            if (!result.has_more ||
                remote_file_pages_loaded_ >=
                    SLEEPHQ_REMOTE_FILE_PAGE_LIMIT) {
                remote_file_cache_complete_ = true;
            }
            Log::logf(CAT_SLEEPHQ,
                      LOG_DEBUG,
                      "remote file page loaded page=%lu count=%u total=%u "
                      "complete=%u\n",
                      static_cast<unsigned long>(remote_file_next_page_ - 1),
                      static_cast<unsigned>(result.count),
                      static_cast<unsigned>(remote_files_.size()),
                      remote_file_cache_complete_ ? 1U : 0U);
            phase_ = WorkPhase::ResolveRemoteFile;
            return ExportStep::Working;

        case WorkPhase::UploadFile:
            current_file_.close(true);
            if (!add_staged_locked(result.upload)) {
                fail_locked("staged_alloc");
                return ExportStep::Idle;
            }
            status_.bytes_uploaded += result.upload.bytes;
            status_.files_uploaded++;
            clear_current_file_locked();
            if (!prepare_inflight_write_locked(InflightPhase::Uploading,
                                               WorkPhase::NextFile)) {
                fail_locked("inflight_write_failed");
                return ExportStep::Idle;
            }
            return ExportStep::Working;

        case WorkPhase::ProcessImport:
            import_process_started_ms_ = nonzero_millis(millis());
            import_poll_due_ms_ = import_process_started_ms_;
            status_.import_status[0] = '\0';
            if (!prepare_inflight_write_locked(InflightPhase::Processing,
                                               WorkPhase::WaitImport)) {
                fail_locked("inflight_write_failed");
                return ExportStep::Idle;
            }
            return ExportStep::Working;

        case WorkPhase::FetchImport: {
            const uint32_t now = nonzero_millis(millis());
            char previous_status[AC_SLEEPHQ_STATUS_MAX] = {};
            copy_cstr(previous_status,
                      sizeof(previous_status),
                      status_.import_status);
            copy_cstr(status_.import_status,
                      sizeof(status_.import_status),
                      result.import.status);
            if (result.import.id) status_.import_id = result.import.id;
            status_.updated_ms = now;

            switch (sleephq_classify_import_status(result.import.status)) {
                case SleepHqImportStatusKind::Success:
                    staged_validation_index_ = 0;
                    phase_ = WorkPhase::ValidateStaged;
                    return ExportStep::Working;
                case SleepHqImportStatusKind::Failure:
                    if (!begin_inflight_remove_locked(
                            InflightRemoveAction::Fail,
                            result.import.failed_reason[0]
                                ? result.import.failed_reason
                                : "import_failed")) {
                        fail_locked("inflight_remove_prepare_failed");
                        return ExportStep::Idle;
                    }
                    return ExportStep::Waiting;
                case SleepHqImportStatusKind::Unknown:
                    if (strcmp(previous_status, result.import.status) != 0) {
                        Log::logf(
                            CAT_SLEEPHQ,
                            LOG_WARN,
                            "unknown import status treated as transient: %s\n",
                            result.import.status[0]
                                ? result.import.status
                                : "<empty>");
                    }
                    break;
                case SleepHqImportStatusKind::Transient:
                    break;
            }

            if (import_process_started_ms_ == 0 ||
                static_cast<uint32_t>(now - import_process_started_ms_) >=
                    SLEEPHQ_IMPORT_POLL_TIMEOUT_MS) {
                fail_locked("import_process_timeout");
                return ExportStep::Idle;
            }
            import_poll_due_ms_ = now + SLEEPHQ_IMPORT_POLL_INTERVAL_MS;
            phase_ = WorkPhase::WaitImport;
            return ExportStep::Waiting;
        }

        case WorkPhase::Idle:
        case WorkPhase::LoadInventory:
        case WorkPhase::LoadInflight:
        case WorkPhase::NextFile:
        case WorkPhase::ReadRebuildMarker:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::WaitImport:
        case WorkPhase::WriteInflight:
        case WorkPhase::ValidateStaged:
        case WorkPhase::FlushState:
        case WorkPhase::WriteRebuildMarker:
        case WorkPhase::WriteDoneMarker:
        case WorkPhase::RemoveInflight:
        case WorkPhase::Finish:
            fail_locked("invalid_blocking_phase");
            return ExportStep::Idle;
    }
    return ExportStep::Idle;
}

ExportStep SleepHqSyncEngine::step_work_phase_locked() {
    switch (phase_) {
        case WorkPhase::Idle:
            phase_ = WorkPhase::Connect;
            return ExportStep::Working;

        case WorkPhase::LoadInventory:
            return step_load_inventory_locked();

        case WorkPhase::LoadInflight:
            return step_load_inflight_locked();

        case WorkPhase::Connect:
        case WorkPhase::ReadIdentification:
        case WorkPhase::FindRemoteMachine:
        case WorkPhase::ResolveDatalogDay:
        case WorkPhase::CreateImport:
        case WorkPhase::OpenLocal:
        case WorkPhase::HashLocalFile:
        case WorkPhase::FetchRemoteFiles:
        case WorkPhase::UploadFile:
        case WorkPhase::ProcessImport:
        case WorkPhase::FetchImport:
            return ExportStep::Waiting;
        case WorkPhase::NextFile:
            return next_file_locked() ? ExportStep::Working : ExportStep::Idle;
        case WorkPhase::ReadRebuildMarker:
            return step_read_rebuild_marker_locked();
        case WorkPhase::ResolveRemoteFile:
            return step_resolve_remote_file_locked();
        case WorkPhase::WaitImport:
            return step_wait_import_locked();
        case WorkPhase::WriteInflight:
            return step_write_inflight_locked();
        case WorkPhase::ValidateStaged:
            return step_validate_staged_locked();
        case WorkPhase::FlushState:
            return step_flush_state_locked();
        case WorkPhase::WriteRebuildMarker:
            return step_write_rebuild_marker_locked();
        case WorkPhase::WriteDoneMarker:
            return step_write_done_marker_locked();
        case WorkPhase::RemoveInflight:
            return step_remove_inflight_locked();
        case WorkPhase::Finish:
            return finish_import_or_sync_locked();
    }
    return ExportStep::Idle;
}

ExportStep SleepHqSyncEngine::step() {
    if (!lock(20)) return ExportStep::Waiting;
    const uint32_t now = nonzero_millis(millis());
    apply_pending_config_locked();
    queue_retry_locked(now);
    if (abort_requested_.load() &&
        status_.state == SleepHqSyncState::Working) {
        fail_locked("preempted");
        unlock();
        return ExportStep::Idle;
    }
    if (status_.pending && phase_ == WorkPhase::Idle) {
        if (StorageService::status().maintenance_active) {
            unlock();
            return ExportStep::Waiting;
        }
        if (!begin_run_locked(now)) {
            unlock();
            return status_.pending ? ExportStep::Waiting : ExportStep::Idle;
        }
    }
    if (status_.state != SleepHqSyncState::Working) {
        unlock();
        return ExportStep::Idle;
    }

    const WorkPhase phase = phase_;
    if (!phase_has_blocking_io(phase)) {
        const ExportStep result = step_work_phase_locked();
        unlock();
        return result;
    }

    const uint32_t operation_generation = operation_generation_.load();
    unlock();

    BlockingResult blocking_result;
    execute_blocking_phase(phase, blocking_result);
    blocking_result.operation_generation = operation_generation;

    if (!lock_ || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) {
        current_file_.close(false);
        client_.disconnect();
        Log::logf(CAT_SLEEPHQ,
                  LOG_ERROR,
                  "state publish lock unavailable phase=%u\n",
                  static_cast<unsigned>(phase));
        return ExportStep::Idle;
    }
    const ExportStep result =
        publish_blocking_phase_locked(phase, blocking_result);
    unlock();
    return result;
}

SleepHqSyncStatus SleepHqSyncEngine::status() const {
    SleepHqSyncStatus out;
    if (lock(20)) {
        out = status_;
        out.retry_due_ms = retry_due_ms_;
        out.retry_attempt = retry_attempt_;
        out.network_available = network_available_.load();
        unlock();
    } else {
        const SleepHqSyncRuntimeStatus runtime = runtime_status();
        out.state = runtime.state;
        out.pending = runtime.pending;
        out.configured = runtime.configured;
        out.network_available = runtime.network_available;
        out.config_generation = runtime.config_generation;
        out.team_id = runtime.team_id;
    }
    if (out.last_sync_epoch != 0 &&
        out.last_sync_epoch > out.last_check_epoch) {
        out.last_check_epoch = out.last_sync_epoch;
    }
    return out;
}

SleepHqSyncRuntimeStatus SleepHqSyncEngine::runtime_status() const {
    SleepHqSyncRuntimeStatus out;
    out.state = static_cast<SleepHqSyncState>(runtime_state_.load());
    out.pending = runtime_pending_.load();
    out.configured = runtime_configured_.load();
    out.network_available = network_available_.load();
    out.config_generation = runtime_config_generation_.load();
    out.team_id = runtime_team_id_.load();
    out.completed_check_generation =
        runtime_completed_check_generation_.load();
    return out;
}

bool SleepHqSyncEngine::active() const {
    return runtime_status().active();
}

}  // namespace aircannect
