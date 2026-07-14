#include "sleephq_sync_job.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ArduinoJson.h>
#include <esp_rom_md5.h>

#include "crc32.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_export_plan.h"
#include "storage_export_state.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t CONFIG_REFRESH_INTERVAL_MS = 1000;
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

void sleep_hq_digest_to_hex(const uint8_t digest[16],
                            char out[AC_SLEEPHQ_CONTENT_HASH_MAX]) {
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = HEX_DIGITS[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0F];
    }
    out[32] = '\0';
}

uint32_t millis_nonzero() {
    const uint32_t now = millis();
    return now == 0 ? 1 : now;
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

bool SleepHqSyncJob::lock(uint32_t timeout_ms) const {
    return lock_ && xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void SleepHqSyncJob::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

void SleepHqSyncJob::publish_runtime_locked() {
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

void SleepHqSyncJob::copy_string(char *dst,
                                 size_t dst_size,
                                 const String &src) {
    copy_cstr(dst, dst_size, src.c_str());
}

SleepHqSyncJob::ConfigSnapshot SleepHqSyncJob::make_config_snapshot(
    const AppConfigData &config) {
    ConfigSnapshot out;
    copy_string(out.client_id, sizeof(out.client_id),
                config.sleephq_client_id);
    copy_string(out.client_secret, sizeof(out.client_secret),
                config.sleephq_client_secret);
    copy_string(out.team_id, sizeof(out.team_id), config.sleephq_team_id);
    copy_string(out.device_id, sizeof(out.device_id),
                config.sleephq_device_id);
    return out;
}

bool SleepHqSyncJob::snapshot_configured(const ConfigSnapshot &config) {
    return config.client_id[0] && config.client_secret[0];
}

SleepHqConfig SleepHqSyncJob::client_config_from_snapshot(
    const ConfigSnapshot &config) {
    SleepHqConfig out;
    copy_cstr(out.client_id, sizeof(out.client_id), config.client_id);
    copy_cstr(out.client_secret, sizeof(out.client_secret),
              config.client_secret);
    copy_cstr(out.team_id, sizeof(out.team_id), config.team_id);
    copy_cstr(out.device_id, sizeof(out.device_id), config.device_id);
    return out;
}

const char *SleepHqSyncJob::inflight_phase_name(InflightPhase phase) {
    switch (phase) {
        case InflightPhase::Uploading: return "uploading";
        case InflightPhase::Processing: return "processing";
        case InflightPhase::None: break;
    }
    return "none";
}

bool SleepHqSyncJob::parse_inflight_phase(const char *text,
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

void SleepHqSyncJob::begin(const AppConfigData &config) {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    refresh_config(config, millis());
}

bool SleepHqSyncJob::config_matches_locked(
    const ConfigSnapshot &config) const {
    const ConfigSnapshot &active =
        pending_config_valid_ ? pending_config_ : config_;
    return strcmp(active.client_id, config.client_id) == 0 &&
           strcmp(active.client_secret, config.client_secret) == 0 &&
           strcmp(active.team_id, config.team_id) == 0 &&
           strcmp(active.device_id, config.device_id) == 0;
}

void SleepHqSyncJob::apply_config_locked(const ConfigSnapshot &config) {
    config_ = config;
    status_.config_generation++;
    status_.configured = snapshot_configured(config_);
    status_.team_id = 0;
    status_.pending = false;
    status_.pending_reason[0] = 0;
    status_.last_error[0] = 0;
    state_dir_[0] = 0;
    state_cache_.clear();
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    status_.state = status_.configured ? SleepHqSyncState::Idle
                                       : SleepHqSyncState::Disabled;
    phase_ = WorkPhase::Idle;
    publish_runtime_locked();
}

void SleepHqSyncJob::apply_pending_config_locked() {
    if (!pending_config_valid_) return;

    const ConfigSnapshot pending = pending_config_;
    pending_config_valid_ = false;
    pending_config_ = ConfigSnapshot();

    if (status_.state == SleepHqSyncState::Working) {
        fail_locked("preempted");
    }
    apply_config_locked(pending);
}

void SleepHqSyncJob::refresh_config(const AppConfigData &config,
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
        if (status_.state == SleepHqSyncState::Working) {
            pending_config_ = snapshot;
            pending_config_valid_ = true;
            request_operation_abort();
        } else {
            apply_config_locked(snapshot);
        }
    }
    unlock();
}

void SleepHqSyncJob::set_network_available(bool available) {
    const bool was_available = network_available_.exchange(available);
    if (was_available && !available) request_operation_abort();
    if (!lock(0)) return;
    status_.network_available = available;
    publish_runtime_locked();
    unlock();
    if (available && !was_available) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
}

void SleepHqSyncJob::set_runtime_blocked(bool blocked) {
    const bool was_blocked = runtime_blocked_.exchange(blocked);
    if (!was_blocked && blocked) request_operation_abort();
    if (was_blocked && !blocked) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
}

bool SleepHqSyncJob::request_locked(RunKind kind,
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
    status_.updated_ms = millis_nonzero();
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    return true;
}

bool SleepHqSyncJob::request_check(const char *reason) {
    if (!lock(0)) return false;
    const bool queued = request_locked(RunKind::Check, reason);
    publish_runtime_locked();
    unlock();
    if (queued) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
    return queued;
}

bool SleepHqSyncJob::request_sync(const char *reason) {
    if (!lock(0)) return false;
    const bool queued = request_locked(RunKind::Sync, reason);
    publish_runtime_locked();
    unlock();
    if (queued) {
        Log::logf(CAT_SLEEPHQ, LOG_INFO, "sync queued reason=%s\n",
                  reason && *reason ? reason : "manual");
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
    return queued;
}

bool SleepHqSyncJob::request_sync_day(const char *day, const char *reason) {
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
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
    return queued;
}

bool SleepHqSyncJob::request_post_therapy_sync() {
    if (!lock(0)) return false;
    const bool queued = request_locked(RunKind::PostTherapySync,
                                       "post_therapy");
    publish_runtime_locked();
    unlock();
    if (queued) {
        Log::logf(CAT_SLEEPHQ, LOG_INFO,
                  "sync queued reason=post_therapy scope=latest_day\n");
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
    return queued;
}

bool SleepHqSyncJob::build_endpoint_state_dir_locked(uint32_t team_id,
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

bool SleepHqSyncJob::begin_run_locked(uint32_t now_ms) {
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

void SleepHqSyncJob::close_local_locked() {
    if (current_file_.local_open) {
        Storage::Guard guard;
        current_file_.local.close();
        current_file_.local_open = false;
    }
}

void SleepHqSyncJob::clear_current_file_locked() {
    close_local_locked();
    current_file_ = CurrentFile();
    status_.current_path[0] = 0;
}

void SleepHqSyncJob::clear_staged_locked() {
    if (staged_) {
        for (size_t i = 0; i < staged_capacity_; ++i) {
            staged_[i].~StagedFile();
        }
        Memory::free(staged_);
    }
    staged_ = nullptr;
    staged_count_ = 0;
    staged_capacity_ = 0;
    mark_index_ = 0;
}

void SleepHqSyncJob::clear_remote_files_locked() {
    remote_files_.clear();
    remote_file_next_page_ = 1;
    remote_file_pages_loaded_ = 0;
    remote_file_cache_complete_ = false;
}

bool SleepHqSyncJob::reserve_remote_dates_locked(size_t needed) {
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

bool SleepHqSyncJob::cache_remote_date_locked(const char *day, bool exists) {
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

bool SleepHqSyncJob::cached_remote_date_exists_locked(const char *day,
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

void SleepHqSyncJob::clear_remote_dates_locked() {
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

bool SleepHqSyncJob::build_datalog_rebuild_marker_path_locked(
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

bool SleepHqSyncJob::read_datalog_rebuild_marker_locked(
    const char *day,
    uint64_t &epoch) const {
    epoch = 0;
    char path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
    if (!build_datalog_rebuild_marker_path_locked(day, path, sizeof(path))) {
        return false;
    }

    char raw[32] = {};
    {
        Storage::Guard guard;
        File file = Storage::open(path, "r");
        if (!file) return false;
        const int read =
            file.read(reinterpret_cast<uint8_t *>(raw), sizeof(raw) - 1);
        file.close();
        if (read <= 0) return false;
        raw[read] = '\0';
    }

    char *end = nullptr;
    const unsigned long long parsed = strtoull(raw, &end, 10);
    if (end == raw) return false;
    epoch = static_cast<uint64_t>(parsed);
    return epoch != 0;
}

bool SleepHqSyncJob::datalog_rebuild_marker_recent_locked(
    const char *day,
    uint64_t now_epoch,
    uint64_t &marker_epoch) const {
    marker_epoch = 0;
    if (now_epoch == 0) return false;
    if (!read_datalog_rebuild_marker_locked(day, marker_epoch)) return false;
    if (marker_epoch > now_epoch) return true;
    return now_epoch - marker_epoch <
           SLEEPHQ_DATALOG_REBUILD_COOLDOWN_SECONDS;
}

bool SleepHqSyncJob::mark_datalog_rebuild_attempt_locked(
    const char *day,
    uint64_t now_epoch) {
    if (now_epoch == 0) return false;
    char path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
    if (!build_datalog_rebuild_marker_path_locked(day, path, sizeof(path))) {
        return false;
    }
    if (!ensure_state_dir_locked()) return false;

    {
        Storage::Guard guard;
        File file = Storage::open(path, "w");
        if (!file) return false;
        const size_t written =
            file.printf("%llu\n",
                        static_cast<unsigned long long>(now_epoch));
        file.close();
        return written != 0;
    }
}

void SleepHqSyncJob::maybe_mark_datalog_rebuild_success_locked() {
    if (!pending_rebuild_day_[0]) return;
    const uint64_t now_epoch = storage_export_current_epoch_seconds_or_zero();
    if (!mark_datalog_rebuild_attempt_locked(pending_rebuild_day_,
                                             now_epoch)) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "remote rebuild marker write failed day=%s\n",
                  pending_rebuild_day_);
    }
    pending_rebuild_day_[0] = '\0';
}

bool SleepHqSyncJob::force_remote_missing_datalog_day_locked(
    const char *day,
    bool local_complete,
    bool &force_export) {
    force_export = false;
    const uint64_t now_epoch = storage_export_current_epoch_seconds_or_zero();
    uint64_t marker_epoch = 0;
    if (local_complete &&
        datalog_rebuild_marker_recent_locked(day, now_epoch, marker_epoch)) {
        Log::logf(CAT_SLEEPHQ, LOG_DEBUG,
                  "remote machine-date still missing day=%s serial=%s; "
                  "rebuild suppressed marker_epoch=%llu\n",
                  day,
                  remote_serial_,
                  static_cast<unsigned long long>(marker_epoch));
        return true;
    }

    if (local_complete) {
        copy_cstr(pending_rebuild_day_, sizeof(pending_rebuild_day_), day);
    }
    force_export = true;
    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "remote machine-date missing day=%s serial=%s; rebuilding day\n",
              day,
              remote_serial_);
    return true;
}

void SleepHqSyncJob::reset_run_locked(bool keep_status) {
    client_.disconnect();
    close_local_locked();
    export_planner_.reset();
    clear_current_file_locked();
    clear_staged_locked();
    clear_remote_files_locked();
    clear_remote_dates_locked();
    state_cache_.clear();
    phase_ = WorkPhase::Idle;
    import_batch_active_ = false;
    import_process_started_ms_ = 0;
    import_poll_due_ms_ = 0;
    inflight_phase_ = InflightPhase::None;
    pending_rebuild_day_[0] = '\0';
    pending_done_day_[0] = '\0';
    pending_remote_day_[0] = '\0';
    pending_remote_day_local_complete_ = false;
    pending_datalog_day_[0] = '\0';
    current_datalog_day_filter_[0] = '\0';
    latest_datalog_day_[0] = '\0';
    state_dir_[0] = 0;
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

void SleepHqSyncJob::finish_check_locked(uint32_t team_id) {
    status_.team_id = team_id;
    status_.last_check_epoch = storage_export_current_epoch_seconds_or_zero();
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    status_.state = status_.configured ? SleepHqSyncState::Idle
                                       : SleepHqSyncState::Disabled;
    status_.pending_reason[0] = 0;
    status_.updated_ms = millis_nonzero();
    Log::logf(CAT_SLEEPHQ, LOG_INFO, "check ok team_id=%lu\n",
              static_cast<unsigned long>(team_id));
    reset_run_locked(true);
    publish_runtime_locked();
}

void SleepHqSyncJob::finish_sync_locked() {
    remove_inflight_locked();
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
    status_.updated_ms = millis_nonzero();
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

void SleepHqSyncJob::fail_locked(const char *error) {
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
        status_.updated_ms = millis_nonzero();
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
    status_.updated_ms = millis_nonzero();
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
              current_file_.path[0] ? current_file_.path : "--",
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

void SleepHqSyncJob::queue_retry_locked(uint32_t now_ms) {
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

bool SleepHqSyncJob::begin_export_planner_locked(char *error_out,
                                                 size_t error_out_size) {
    refresh_latest_datalog_day_name_locked();
    StorageExportPlannerConfig config;
    config.scope = StorageExportPlannerScope::SleepHq;
    config.state_dir = state_dir_;
    config.state_cache = &state_cache_;
    config.latest_datalog_day = latest_datalog_day_;
    config.trust_completed_finalized_datalog_days = true;
    config.require_pending_datalog_file = true;
    config.defer_datalog_day_decision = true;
    if (current_run_kind_ == RunKind::PostTherapySync) {
        config.max_datalog_days = SLEEPHQ_POST_THERAPY_DATALOG_DAY_LIMIT;
    }
    if (current_datalog_day_filter_[0]) {
        config.only_datalog_day = current_datalog_day_filter_;
    }
    return export_planner_.begin(config, error_out, error_out_size);
}

void SleepHqSyncJob::reset_import_batch_locked() {
    remove_inflight_locked();
    close_local_locked();
    clear_current_file_locked();
    clear_staged_locked();
    status_.import_id = 0;
    status_.import_status[0] = '\0';
    import_process_started_ms_ = 0;
    import_poll_due_ms_ = 0;
    mark_index_ = 0;
    pending_rebuild_day_[0] = '\0';
    pending_done_day_[0] = '\0';
    import_batch_active_ = false;
    phase_ = WorkPhase::NextFile;
}

JobStep SleepHqSyncJob::finish_import_or_sync_locked() {
    if (import_batch_active_) {
        reset_import_batch_locked();
        return JobStep::Working;
    }
    finish_sync_locked();
    return JobStep::Idle;
}

bool SleepHqSyncJob::next_file_locked() {
    if (runtime_blocked_.load()) {
        fail_locked("preempted");
        return false;
    }
    if (!Storage::mounted()) {
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
                phase_ = staged_count_ == 0 ? WorkPhase::NextFile
                                            : WorkPhase::ProcessImport;
                if (staged_count_ == 0) {
                    maybe_mark_completed_datalog_day_locked();
                }
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
            phase_ = needs_lookup ? WorkPhase::ResolveDatalogDay
                                  : WorkPhase::NextFile;
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

bool SleepHqSyncJob::build_sleep_path_locked(const char *local_path,
                                             char *path_out,
                                             size_t path_out_size,
                                             char *name_out,
                                             size_t name_out_size) const {
    return storage_export_build_relative_file_path(local_path,
                                                   path_out,
                                                   path_out_size,
                                                   name_out,
                                                   name_out_size);
}

bool SleepHqSyncJob::current_file_matches_snapshot() const {
    const StorageLocalNodeInfo info = storage_stat_local_node(current_file_.path);
    return info.exists && !info.is_dir &&
           info.size == current_file_.size &&
           info.mtime == current_file_.mtime;
}

bool SleepHqSyncJob::compute_current_file_content_hash(char *out,
                                                       size_t out_size) {
    if (!out || out_size < AC_SLEEPHQ_CONTENT_HASH_MAX ||
        !current_file_.local_open || !current_file_.name[0]) {
        return false;
    }
    uint8_t *buffer = static_cast<uint8_t *>(Memory::alloc_large(4096, false));
    if (!buffer) {
        Log::logf(CAT_SLEEPHQ, LOG_ERROR,
                  "hash buffer allocation failed bytes=4096\n");
        return false;
    }

    md5_context_t md5;
    esp_rom_md5_init(&md5);
    uint64_t read_total = 0;
    bool ok = true;
    {
        Storage::Guard guard;
        ok = current_file_.local.seek(0);
    }
    while (ok && read_total < current_file_.size) {
        const uint64_t remaining = current_file_.size - read_total;
        const size_t wanted =
            remaining > 4096 ? 4096 : static_cast<size_t>(remaining);
        size_t read = 0;
        {
            Storage::Guard guard;
            read = current_file_.local.read(buffer, wanted);
        }
        if (read != wanted) {
            ok = false;
            break;
        }
        esp_rom_md5_update(&md5, buffer, read);
        read_total += read;
        taskYIELD();
    }
    Memory::free(buffer);
    if (!ok) return false;

    esp_rom_md5_update(&md5,
                       reinterpret_cast<const uint8_t *>(current_file_.name),
                       strlen(current_file_.name));
    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5);
    sleep_hq_digest_to_hex(digest, out);
    {
        Storage::Guard guard;
        ok = current_file_.local.seek(0);
    }
    current_file_.offset = 0;
    return ok;
}

bool SleepHqSyncJob::plan_file_locked(const StorageExportPlannerItem &item) {
    const char *path = item.path;
    clear_current_file_locked();
    copy_cstr(current_file_.path, sizeof(current_file_.path), path);
    copy_cstr(status_.current_path, sizeof(status_.current_path), path);
    current_file_.force_upload = item.force_export;
    status_.files_seen++;
    status_.updated_ms = millis_nonzero();

    if (!item.info.exists || item.info.is_dir) {
        phase_ = WorkPhase::NextFile;
        return true;
    }
    current_file_.size = item.info.size;
    current_file_.mtime = item.info.mtime;
    if (!build_sleep_path_locked(path,
                                 current_file_.sleep_path,
                                 sizeof(current_file_.sleep_path),
                                 current_file_.name,
                                 sizeof(current_file_.name))) {
        fail_locked("sleep_path_failed");
        return false;
    }
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
    const bool state_has_file = item.local_state_complete;
    if (!current_file_.force_upload && state_has_file) {
        status_.files_skipped++;
        clear_current_file_locked();
        phase_ = WorkPhase::NextFile;
        return true;
    }
    current_file_.attach_by_hash = current_file_.force_upload && state_has_file;
    if (status_.import_id != 0 &&
        staged_contains_locked(path, current_file_.size,
                               current_file_.mtime)) {
        status_.files_skipped++;
        clear_current_file_locked();
        phase_ = WorkPhase::NextFile;
        return true;
    }
    phase_ = status_.import_id == 0 ? WorkPhase::CreateImport
                                    : WorkPhase::OpenLocal;
    import_batch_active_ = true;
    return true;
}

bool SleepHqSyncJob::ensure_state_dir_locked() {
    return storage_export_ensure_state_dir(state_dir_);
}

bool SleepHqSyncJob::build_inflight_path_locked(char *out,
                                                size_t out_size) const {
    if (!state_dir_[0] || !out || out_size == 0) return false;
    const int written = snprintf(out, out_size, "%s/%s",
                                 state_dir_, SLEEPHQ_INFLIGHT_FILE);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool SleepHqSyncJob::staged_contains_locked(const char *path,
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

void SleepHqSyncJob::refresh_latest_datalog_day_name_locked() {
    latest_datalog_day_[0] = '\0';
    char path[AC_STORAGE_PATH_MAX] = {};
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    if (!storage_export_latest_datalog_day_path(path, sizeof(path),
                                                error, sizeof(error))) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "latest DATALOG day scan failed error=%s\n",
                  error[0] ? error : "latest_day_failed");
        return;
    }
    (void)storage_export_datalog_day_from_path(path,
                                               latest_datalog_day_,
                                               sizeof(latest_datalog_day_));
}

void SleepHqSyncJob::note_completed_datalog_day_locked(const char *day) {
    pending_done_day_[0] = '\0';
    if (!storage_export_datalog_day_finalized(day, latest_datalog_day_)) return;
    if (storage_export_datalog_day_done(state_dir_, day)) return;
    copy_cstr(pending_done_day_, sizeof(pending_done_day_), day);
}

void SleepHqSyncJob::maybe_mark_completed_datalog_day_locked() {
    if (!pending_done_day_[0]) return;
    if (!storage_export_mark_datalog_day_done(state_dir_,
                                              pending_done_day_)) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "DATALOG done marker write failed day=%s\n",
                  pending_done_day_);
        pending_done_day_[0] = '\0';
        return;
    }
    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "DATALOG day complete day=%s\n",
              pending_done_day_);
    pending_done_day_[0] = '\0';
}

bool SleepHqSyncJob::write_inflight_locked(InflightPhase phase) {
    if (!ensure_state_dir_locked() || !status_.team_id ||
        !status_.import_id || phase == InflightPhase::None) {
        return false;
    }
    char path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
    if (!build_inflight_path_locked(path, sizeof(path))) return false;
    File file;
    {
        Storage::Guard guard;
        file = Storage::open(path, "w");
        if (!file) return false;
        size_t written =
            file.printf("v1\t%lu\t%lu\t%s\n",
                        static_cast<unsigned long>(status_.team_id),
                        static_cast<unsigned long>(status_.import_id),
                        inflight_phase_name(phase));
        if (written == 0) {
            file.close();
            return false;
        }
        for (size_t i = 0; i < staged_count_; ++i) {
            const StagedFile &entry = staged_[i];
            const char mode =
                entry.state_write_mode == StateWriteMode::Replace ? 'r' : 'a';
            written =
                file.printf("%llu\t%llu\t%s\t%lu\t%c\t%s\t%s\n",
                            static_cast<unsigned long long>(entry.size),
                            static_cast<unsigned long long>(entry.mtime),
                            entry.content_hash,
                            static_cast<unsigned long>(entry.import_id),
                            mode,
                            entry.path,
                            entry.state_path);
            if (written == 0) {
                file.close();
                return false;
            }
        }
        file.close();
    }
    inflight_phase_ = phase;
    return true;
}

void SleepHqSyncJob::remove_inflight_locked() {
    char path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
    if (!build_inflight_path_locked(path, sizeof(path))) return;
    (void)Storage::remove(path);
    inflight_phase_ = InflightPhase::None;
}

bool SleepHqSyncJob::load_inflight_locked(InflightPhase &phase_out) {
    phase_out = InflightPhase::None;
    clear_staged_locked();
    if (!state_dir_[0]) return true;

    char path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
    if (!build_inflight_path_locked(path, sizeof(path))) return false;

    File file;
    {
        Storage::Guard guard;
        file = Storage::open(path, "r");
    }
    if (!file) return true;

    uint8_t buffer[512] = {};
    char line[AC_STORAGE_PATH_MAX * 2 + 160] = {};
    size_t line_len = 0;
    bool header_seen = false;
    bool ok = true;

    auto parse_line = [&](char *text) -> bool {
        if (!text || !text[0]) return true;
        if (!header_seen) {
            char *version = strtok(text, "\t");
            char *team = strtok(nullptr, "\t");
            char *import_id = strtok(nullptr, "\t");
            char *phase = strtok(nullptr, "\t");
            if (!version || !team || !import_id || !phase ||
                strcmp(version, "v1") != 0) {
                return false;
            }
            char *end = nullptr;
            const unsigned long parsed_team = strtoul(team, &end, 10);
            if (!end || *end != '\0' || parsed_team == 0) return false;
            end = nullptr;
            const unsigned long parsed_import = strtoul(import_id, &end, 10);
            if (!end || *end != '\0' || parsed_import == 0) return false;
            InflightPhase parsed_phase = InflightPhase::None;
            if (!parse_inflight_phase(phase, parsed_phase) ||
                parsed_phase == InflightPhase::None) {
                return false;
            }
            if (status_.team_id != 0 &&
                status_.team_id != static_cast<uint32_t>(parsed_team)) {
                return false;
            }
            status_.team_id = static_cast<uint32_t>(parsed_team);
            status_.import_id = static_cast<uint32_t>(parsed_import);
            phase_out = parsed_phase;
            header_seen = true;
            return true;
        }

        char *size_text = strtok(text, "\t");
        char *mtime_text = strtok(nullptr, "\t");
        char *hash = strtok(nullptr, "\t");
        char *import_text = strtok(nullptr, "\t");
        char *mode_text = strtok(nullptr, "\t");
        char *local_path = strtok(nullptr, "\t");
        char *state_path = strtok(nullptr, "\t");
        if (!size_text || !mtime_text || !hash || !import_text ||
            !mode_text || !local_path || !state_path) {
            return false;
        }
        char *end = nullptr;
        const unsigned long long parsed_size = strtoull(size_text, &end, 10);
        if (!end || *end != '\0') return false;
        end = nullptr;
        const unsigned long long parsed_mtime = strtoull(mtime_text, &end, 10);
        if (!end || *end != '\0') return false;
        end = nullptr;
        const unsigned long parsed_import = strtoul(import_text, &end, 10);
        if (!end || *end != '\0' || parsed_import == 0 ||
            parsed_import != status_.import_id) {
            return false;
        }
        if (strlen(hash) >= AC_SLEEPHQ_CONTENT_HASH_MAX ||
            (mode_text[0] != 'a' && mode_text[0] != 'r') ||
            mode_text[1] != '\0') {
            return false;
        }
        if (!reserve_staged_locked(staged_count_ + 1)) return false;
        StagedFile &entry = staged_[staged_count_++];
        entry.size = static_cast<uint64_t>(parsed_size);
        entry.mtime = static_cast<uint64_t>(parsed_mtime);
        entry.import_id = static_cast<uint32_t>(parsed_import);
        entry.state_write_mode =
            mode_text[0] == 'r' ? StateWriteMode::Replace
                                : StateWriteMode::Append;
        copy_cstr(entry.content_hash, sizeof(entry.content_hash), hash);
        copy_cstr(entry.path, sizeof(entry.path), local_path);
        copy_cstr(entry.state_path, sizeof(entry.state_path), state_path);
        return true;
    };

    for (;;) {
        size_t read = 0;
        {
            Storage::Guard guard;
            read = file.read(buffer, sizeof(buffer));
        }
        if (read == 0) break;
        for (size_t i = 0; i < read; ++i) {
            const char ch = static_cast<char>(buffer[i]);
            if (ch == '\n') {
                line[line_len] = '\0';
                if (!parse_line(line)) {
                    ok = false;
                    break;
                }
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) break;
    }
    if (ok && line_len > 0) {
        line[line_len] = '\0';
        ok = parse_line(line);
    }
    {
        Storage::Guard guard;
        file.close();
    }
    if (!ok || !header_seen) {
        clear_staged_locked();
        status_.import_id = 0;
        phase_out = InflightPhase::None;
        inflight_phase_ = InflightPhase::None;
        return false;
    }
    inflight_phase_ = phase_out;
    status_.files_uploaded = staged_count_;
    status_.bytes_uploaded = 0;
    for (size_t i = 0; i < staged_count_; ++i) {
        status_.bytes_uploaded += staged_[i].size;
    }
    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "resuming import=%lu phase=%s files=%u\n",
              static_cast<unsigned long>(status_.import_id),
              inflight_phase_name(phase_out),
              static_cast<unsigned>(staged_count_));
    return true;
}

bool SleepHqSyncJob::write_state_locked(const StagedFile &file_info) {
    char line[AC_STORAGE_PATH_MAX + AC_SLEEPHQ_CONTENT_HASH_MAX + 96] = {};
    const int written =
        snprintf(line,
                 sizeof(line),
                 "%llu\t%llu\t%s\t%lu\t%s",
                 static_cast<unsigned long long>(file_info.size),
                 static_cast<unsigned long long>(file_info.mtime),
                 file_info.content_hash,
                 static_cast<unsigned long>(file_info.import_id),
                 file_info.path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(line)) {
        return false;
    }
    const StorageExportStateWriteMode mode =
        file_info.state_write_mode == StateWriteMode::Append
            ? StorageExportStateWriteMode::Append
            : StorageExportStateWriteMode::Replace;
    return storage_export_write_state_line(&state_cache_,
                                           state_dir_,
                                           file_info.state_path,
                                           file_info.path,
                                           file_info.size,
                                           file_info.mtime,
                                           mode,
                                           line,
                                           true);
}

bool SleepHqSyncJob::reserve_staged_locked(size_t needed) {
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

bool SleepHqSyncJob::add_staged_locked(const SleepHqUploadResult &upload) {
    if (!reserve_staged_locked(staged_count_ + 1)) return false;
    StagedFile &entry = staged_[staged_count_++];
    copy_cstr(entry.path, sizeof(entry.path), current_file_.path);
    copy_cstr(entry.state_path, sizeof(entry.state_path),
              current_file_.state_path);
    copy_cstr(entry.content_hash, sizeof(entry.content_hash),
              upload.content_hash);
    entry.size = current_file_.size;
    entry.mtime = current_file_.mtime;
    entry.import_id = status_.import_id;
    entry.state_write_mode = current_file_.state_write_mode;
    return true;
}

bool SleepHqSyncJob::remote_file_cache_contains_locked(
    const CurrentFile &file) const {
    if (!file.name[0] || !file.sleep_path[0] || !file.content_hash[0]) {
        return false;
    }
    return remote_files_.contains(file.name, file.sleep_path,
                                  file.content_hash, file.size);
}

bool SleepHqSyncJob::remote_file_list_cb(void *ctx,
                                         const SleepHqRemoteFile &file) {
    SleepHqRemoteFileCache *cache =
        static_cast<SleepHqRemoteFileCache *>(ctx);
    return cache && cache->add(file);
}

bool SleepHqSyncJob::remote_machine_list_cb(void *ctx,
                                            const SleepHqMachine &machine) {
    MachineListContext *lookup = static_cast<MachineListContext *>(ctx);
    if (!lookup || lookup->machine_id || !lookup->serial) return lookup != nullptr;
    if (machine.id && strcmp(machine.serial_number, lookup->serial) == 0) {
        lookup->machine_id = machine.id;
    }
    return true;
}

bool SleepHqSyncJob::read_local_machine_serial_locked(char *out,
                                                      size_t out_size,
                                                      char *error,
                                                      size_t error_size) {
    if (!out || out_size == 0) {
        copy_cstr(error, error_size, "bad_serial_buffer");
        return false;
    }
    out[0] = '\0';
    const StorageLocalNodeInfo info =
        storage_stat_local_node(SLEEPHQ_IDENTIFICATION_PATH);
    if (!info.exists) return true;
    if (info.is_dir || info.size == 0 ||
        info.size > SLEEPHQ_IDENTIFICATION_JSON_MAX) {
        copy_cstr(error, error_size, "identification_invalid");
        return false;
    }

    const size_t size = static_cast<size_t>(info.size);
    char *json = static_cast<char *>(Memory::alloc_large(size + 1, false));
    if (!json) {
        copy_cstr(error, error_size, "identification_alloc");
        return false;
    }

    bool ok = true;
    {
        Storage::Guard guard;
        File file = Storage::open(SLEEPHQ_IDENTIFICATION_PATH, "r");
        if (!file || file.isDirectory()) {
            ok = false;
        } else {
            const size_t read = file.read(reinterpret_cast<uint8_t *>(json),
                                          size);
            file.close();
            ok = read == size;
        }
    }
    if (!ok) {
        Memory::free(json);
        copy_cstr(error, error_size, "identification_read");
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

void SleepHqSyncJob::note_remote_machine_missing_locked() {
    if (remote_reconcile_all_missing_) return;
    remote_reconcile_all_missing_ = true;
    Log::logf(CAT_SLEEPHQ, LOG_INFO,
              "remote machine missing serial=%s; pending DATALOG days will rebuild\n",
              remote_serial_);
}

bool SleepHqSyncJob::prepare_remote_reconcile_locked(char *error,
                                                     size_t error_size) {
    remote_machine_id_ = 0;
    remote_machine_next_page_ = 1;
    remote_machine_pages_loaded_ = 0;
    remote_reconcile_enabled_ = false;
    remote_reconcile_all_missing_ = false;
    remote_serial_[0] = '\0';
    remote_date_count_ = 0;

    char serial[AC_SLEEPHQ_SERIAL_MAX] = {};
    if (!read_local_machine_serial_locked(serial, sizeof(serial),
                                          error, error_size)) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "remote reconcile disabled: %s\n",
                  error && error[0] ? error : "identification_failed");
        error[0] = '\0';
        return true;
    }
    if (!serial[0]) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "remote reconcile disabled: identification serial missing\n");
        return true;
    }

    copy_cstr(remote_serial_, sizeof(remote_serial_), serial);
    remote_reconcile_enabled_ = true;
    return true;
}

bool SleepHqSyncJob::resolve_pending_datalog_day_locked(
    bool &needs_lookup,
    char *error,
    size_t error_size) {
    needs_lookup = false;
    if (!pending_remote_day_[0]) {
        copy_cstr(error, error_size, "bad_datalog_day");
        return false;
    }

    bool force_export = false;
    if (!remote_reconcile_enabled_) {
        const bool resolved = export_planner_.resolve_datalog_day_decision(
            false, error, error_size);
        if (resolved) {
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
        }
        return resolved;
    }
    if (remote_reconcile_all_missing_) {
        const bool resolved = export_planner_.resolve_datalog_day_decision(
            true, error, error_size);
        if (resolved) {
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
        }
        return resolved;
    }

    bool exists = false;
    if (cached_remote_date_exists_locked(pending_remote_day_, exists)) {
        if (!exists &&
            !force_remote_missing_datalog_day_locked(
                pending_remote_day_,
                pending_remote_day_local_complete_,
                force_export)) {
            copy_cstr(error, error_size, "remote_date_decision_failed");
            return false;
        }
        const bool resolved = export_planner_.resolve_datalog_day_decision(
            force_export, error, error_size);
        if (resolved) {
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
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

JobStep SleepHqSyncJob::begin_export_work_locked() {
    char planner_error[AC_SLEEPHQ_ERROR_MAX] = {};
    if (!begin_export_planner_locked(planner_error, sizeof(planner_error))) {
        fail_locked(planner_error[0] ? planner_error : "planner_failed");
        return JobStep::Idle;
    }
    InflightPhase restored_phase = InflightPhase::None;
    if (!load_inflight_locked(restored_phase)) {
        remove_inflight_locked();
        clear_staged_locked();
        status_.import_id = 0;
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "discarded corrupt inflight state\n");
    }
    if (restored_phase != InflightPhase::None && status_.import_id != 0) {
        import_batch_active_ = true;
    }
    if (restored_phase == InflightPhase::Processing &&
        status_.import_id != 0) {
        import_process_started_ms_ = millis_nonzero();
        import_poll_due_ms_ = import_process_started_ms_;
        phase_ = WorkPhase::WaitImport;
        return JobStep::Waiting;
    }
    phase_ = WorkPhase::NextFile;
    return JobStep::Working;
}

JobStep SleepHqSyncJob::step_resolve_remote_file_locked() {
    if (!current_file_.content_hash[0]) {
        phase_ = WorkPhase::HashLocalFile;
        return JobStep::Working;
    }

    if (current_file_.attach_by_hash ||
        remote_file_cache_contains_locked(current_file_)) {
        current_file_.attach_by_hash = true;
        phase_ = WorkPhase::UploadFile;
        return JobStep::Working;
    }

    if (!remote_file_cache_complete_ &&
        remote_file_pages_loaded_ < SLEEPHQ_REMOTE_FILE_PAGE_LIMIT) {
        phase_ = WorkPhase::FetchRemoteFiles;
        return JobStep::Working;
    }

    remote_file_cache_complete_ = true;
    phase_ = WorkPhase::UploadFile;
    return JobStep::Working;
}

bool SleepHqSyncJob::upload_read_cb(void *ctx,
                                    uint8_t *out,
                                    size_t len,
                                    size_t &read) {
    UploadContext *upload = static_cast<UploadContext *>(ctx);
    read = 0;
    if (!upload || !upload->file || !out ||
        (upload->abort_requested && upload->abort_requested->load())) {
        return false;
    }

    Storage::Guard guard;
    read = upload->file->read(out, len);
    upload->offset += static_cast<uint64_t>(read);
    return read == len;
}

bool SleepHqSyncJob::upload_reset_cb(void *ctx) {
    UploadContext *upload = static_cast<UploadContext *>(ctx);
    if (!upload || !upload->file) return false;

    Storage::Guard guard;
    const bool ok = upload->file->seek(0);
    if (ok) upload->offset = 0;
    return ok;
}

bool SleepHqSyncJob::operation_abort_cb(void *ctx) {
    SleepHqSyncJob *job = static_cast<SleepHqSyncJob *>(ctx);
    return !job || job->abort_requested_.load() ||
           job->runtime_blocked_.load();
}

BackgroundOperationControl SleepHqSyncJob::operation_control(
    uint32_t timeout_ms) const {
    BackgroundOperationControl operation;
    operation.started_ms = millis();
    operation.timeout_ms = timeout_ms;
    operation.should_abort = &SleepHqSyncJob::operation_abort_cb;
    operation.ctx = const_cast<SleepHqSyncJob *>(this);
    return operation;
}

void SleepHqSyncJob::request_operation_abort() {
    bool expected = false;
    if (abort_requested_.compare_exchange_strong(expected, true)) {
        operation_generation_.fetch_add(1);
    }
}

JobStep SleepHqSyncJob::step_wait_import_locked() {
    if (staged_count_ == 0) {
        phase_ = WorkPhase::Finish;
        return JobStep::Working;
    }

    const uint32_t now = millis_nonzero();
    if (import_poll_due_ms_ != 0 &&
        static_cast<int32_t>(now - import_poll_due_ms_) < 0) {
        return JobStep::Waiting;
    }
    phase_ = WorkPhase::FetchImport;
    return JobStep::Working;
}

JobStep SleepHqSyncJob::step_mark_state_locked(char *error,
                                               size_t error_size) {
    if (mark_index_ >= staged_count_) {
        maybe_mark_datalog_rebuild_success_locked();
        maybe_mark_completed_datalog_day_locked();
        phase_ = WorkPhase::Finish;
        return JobStep::Working;
    }
    const StagedFile &file = staged_[mark_index_];
    if (!write_state_locked(file)) {
        copy_cstr(error, error_size, "state_write_failed");
        fail_locked(error);
        return JobStep::Idle;
    }
    mark_index_++;
    return JobStep::Working;
}

bool SleepHqSyncJob::phase_has_blocking_io(WorkPhase phase) {
    switch (phase) {
        case WorkPhase::Connect:
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
        case WorkPhase::NextFile:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::WaitImport:
        case WorkPhase::MarkState:
        case WorkPhase::Finish:
            return false;
    }
    return false;
}

void SleepHqSyncJob::execute_blocking_phase(WorkPhase phase,
                                            BlockingResult &result) {
    BackgroundOperationControl operation =
        operation_control(SLEEPHQ_API_OPERATION_TIMEOUT_MS);

    switch (phase) {
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
                &SleepHqSyncJob::remote_machine_list_cb,
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
            Storage::Guard guard;
            result.local = Storage::open(current_file_.path, "r");
            result.ok = result.local && !result.local.isDirectory();
            if (!result.ok) {
                if (result.local) result.local.close();
                copy_cstr(result.error,
                          sizeof(result.error),
                          "local_open_failed");
            }
            return;
        }

        case WorkPhase::HashLocalFile:
            result.ok = compute_current_file_content_hash(
                result.content_hash,
                sizeof(result.content_hash));
            if (!result.ok) {
                copy_cstr(result.error, sizeof(result.error), "hash_failed");
            }
            return;

        case WorkPhase::FetchRemoteFiles:
            result.performed = true;
            result.ok = client_.list_team_files(
                status_.team_id,
                remote_file_next_page_,
                SLEEPHQ_REMOTE_FILE_PER_PAGE,
                &SleepHqSyncJob::remote_file_list_cb,
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
            if (!current_file_matches_snapshot()) {
                copy_cstr(result.error,
                          sizeof(result.error),
                          "local_changed");
                return;
            }

            UploadContext upload_context;
            upload_context.file = &current_file_.local;
            upload_context.abort_requested = &abort_requested_;
            bool attached = false;
            operation.timeout_ms =
                sleep_hq_upload_timeout_ms(current_file_.size);
            operation.started_ms = millis();

            if (current_file_.attach_by_hash) {
                SleepHqAttachRequest attach;
                attach.import_id = status_.import_id;
                attach.name = current_file_.name;
                attach.path = current_file_.sleep_path;
                attach.content_hash = current_file_.content_hash;
                if (client_.attach_file(attach, result.upload, &operation)) {
                    attached = true;
                } else if (!upload_reset_cb(&upload_context)) {
                    copy_cstr(result.error,
                              sizeof(result.error),
                              client_.last_error());
                    return;
                }
            }

            if (!attached) {
                SleepHqUploadRequest request;
                request.import_id = status_.import_id;
                request.name = current_file_.name;
                request.path = current_file_.sleep_path;
                request.content_hash = current_file_.content_hash[0]
                    ? current_file_.content_hash
                    : nullptr;
                request.size = current_file_.size;
                request.read = &SleepHqSyncJob::upload_read_cb;
                request.reset = &SleepHqSyncJob::upload_reset_cb;
                request.ctx = &upload_context;
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
        case WorkPhase::NextFile:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::WaitImport:
        case WorkPhase::MarkState:
        case WorkPhase::Finish:
            copy_cstr(result.error,
                      sizeof(result.error),
                      "invalid_blocking_phase");
            return;
    }
}

JobStep SleepHqSyncJob::publish_blocking_phase_locked(
    WorkPhase phase,
    BlockingResult &result) {
    if (status_.state != SleepHqSyncState::Working || phase_ != phase) {
        if (result.local) result.local.close();
        client_.disconnect();
        return JobStep::Idle;
    }

    if (!background_operation_result_current(
            result.operation_generation,
            operation_generation_.load(),
            abort_requested_.load()) ||
        pending_config_valid_ ||
        runtime_blocked_.load() ||
        !network_available_.load()) {
        if (result.local) result.local.close();
        fail_locked("preempted");
        apply_pending_config_locked();
        return status_.pending ? JobStep::Waiting : JobStep::Idle;
    }

    if (!result.ok) {
        if (result.local) result.local.close();
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
                return JobStep::Idle;
            }
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
            phase_ = WorkPhase::NextFile;
            return JobStep::Working;
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
            return JobStep::Working;
        }

        if (phase == WorkPhase::FetchImport &&
            strcmp(result.error, "http_404") == 0) {
            remove_inflight_locked();
        }
        fail_locked(result.error[0] ? result.error : "network_io_failed");
        return JobStep::Idle;
    }

    switch (phase) {
        case WorkPhase::Connect: {
            status_.team_id = result.team_id;
            if (current_run_kind_ == RunKind::Check) {
                finish_check_locked(result.team_id);
                return JobStep::Idle;
            }
            if (!build_endpoint_state_dir_locked(result.team_id,
                                                 state_dir_,
                                                 sizeof(state_dir_))) {
                fail_locked("state_path_failed");
                return JobStep::Idle;
            }

            char error[AC_SLEEPHQ_ERROR_MAX] = {};
            if (!prepare_remote_reconcile_locked(error, sizeof(error))) {
                fail_locked(error[0] ? error : "remote_reconcile_failed");
                return JobStep::Idle;
            }
            phase_ = WorkPhase::FindRemoteMachine;
            return JobStep::Working;
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
            return JobStep::Working;

        case WorkPhase::ResolveDatalogDay: {
            if (!cache_remote_date_locked(pending_remote_day_,
                                          result.remote_date_exists)) {
                fail_locked("remote_date_cache_alloc");
                return JobStep::Idle;
            }

            bool force_export = false;
            if (!result.remote_date_exists &&
                !force_remote_missing_datalog_day_locked(
                    pending_remote_day_,
                    pending_remote_day_local_complete_,
                    force_export)) {
                fail_locked("remote_date_decision_failed");
                return JobStep::Idle;
            }

            char error[AC_SLEEPHQ_ERROR_MAX] = {};
            if (!export_planner_.resolve_datalog_day_decision(
                    force_export,
                    error,
                    sizeof(error))) {
                fail_locked(error[0] ? error : "planner_decision_failed");
                return JobStep::Idle;
            }
            pending_remote_day_[0] = '\0';
            pending_remote_day_local_complete_ = false;
            phase_ = WorkPhase::NextFile;
            return JobStep::Working;
        }

        case WorkPhase::CreateImport:
            if (!result.import.id) {
                fail_locked("import_id_missing");
                return JobStep::Idle;
            }
            status_.import_id = result.import.id;
            if (!write_inflight_locked(InflightPhase::Uploading)) {
                fail_locked("inflight_write_failed");
                return JobStep::Idle;
            }
            phase_ = WorkPhase::OpenLocal;
            return JobStep::Working;

        case WorkPhase::OpenLocal:
            current_file_.local = result.local;
            result.local = File();
            current_file_.local_open = true;
            current_file_.offset = 0;
            phase_ = WorkPhase::ResolveRemoteFile;
            return JobStep::Working;

        case WorkPhase::HashLocalFile:
            copy_cstr(current_file_.content_hash,
                      sizeof(current_file_.content_hash),
                      result.content_hash);
            current_file_.offset = 0;
            phase_ = WorkPhase::ResolveRemoteFile;
            return JobStep::Working;

        case WorkPhase::FetchRemoteFiles:
            if (!remote_files_.merge_from(result.remote_files)) {
                fail_locked("remote_file_cache_alloc");
                return JobStep::Idle;
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
            return JobStep::Working;

        case WorkPhase::UploadFile:
            close_local_locked();
            if (!add_staged_locked(result.upload)) {
                fail_locked("staged_alloc");
                return JobStep::Idle;
            }
            if (!write_inflight_locked(InflightPhase::Uploading)) {
                fail_locked("inflight_write_failed");
                return JobStep::Idle;
            }
            status_.bytes_uploaded += result.upload.bytes;
            status_.files_uploaded++;
            clear_current_file_locked();
            phase_ = WorkPhase::NextFile;
            return JobStep::Working;

        case WorkPhase::ProcessImport:
            if (!write_inflight_locked(InflightPhase::Processing)) {
                fail_locked("inflight_write_failed");
                return JobStep::Idle;
            }
            import_process_started_ms_ = millis_nonzero();
            import_poll_due_ms_ = import_process_started_ms_;
            status_.import_status[0] = '\0';
            phase_ = WorkPhase::WaitImport;
            return JobStep::Waiting;

        case WorkPhase::FetchImport: {
            const uint32_t now = millis_nonzero();
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
                    mark_index_ = 0;
                    phase_ = WorkPhase::MarkState;
                    return JobStep::Working;
                case SleepHqImportStatusKind::Failure:
                    remove_inflight_locked();
                    fail_locked(result.import.failed_reason[0]
                                    ? result.import.failed_reason
                                    : "import_failed");
                    return JobStep::Idle;
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
                return JobStep::Idle;
            }
            import_poll_due_ms_ = now + SLEEPHQ_IMPORT_POLL_INTERVAL_MS;
            phase_ = WorkPhase::WaitImport;
            return JobStep::Waiting;
        }

        case WorkPhase::Idle:
        case WorkPhase::NextFile:
        case WorkPhase::ResolveRemoteFile:
        case WorkPhase::WaitImport:
        case WorkPhase::MarkState:
        case WorkPhase::Finish:
            fail_locked("invalid_blocking_phase");
            return JobStep::Idle;
    }
    return JobStep::Idle;
}

JobStep SleepHqSyncJob::step_work_phase_locked() {
    switch (phase_) {
        case WorkPhase::Idle:
            phase_ = WorkPhase::Connect;
            return JobStep::Working;
        case WorkPhase::Connect:
        case WorkPhase::FindRemoteMachine:
        case WorkPhase::ResolveDatalogDay:
        case WorkPhase::CreateImport:
        case WorkPhase::OpenLocal:
        case WorkPhase::HashLocalFile:
        case WorkPhase::FetchRemoteFiles:
        case WorkPhase::UploadFile:
        case WorkPhase::ProcessImport:
        case WorkPhase::FetchImport:
            return JobStep::Waiting;
        case WorkPhase::NextFile:
            return next_file_locked() ? JobStep::Working : JobStep::Idle;
        case WorkPhase::ResolveRemoteFile:
            return step_resolve_remote_file_locked();
        case WorkPhase::WaitImport:
            return step_wait_import_locked();
        case WorkPhase::MarkState: {
            char error[AC_SLEEPHQ_ERROR_MAX] = {};
            return step_mark_state_locked(error, sizeof(error));
        }
        case WorkPhase::Finish:
            return finish_import_or_sync_locked();
    }
    return JobStep::Idle;
}

JobStep SleepHqSyncJob::step() {
    if (!lock(20)) return JobStep::Waiting;
    const uint32_t now = millis_nonzero();
    apply_pending_config_locked();
    queue_retry_locked(now);
    if (abort_requested_.load() &&
        status_.state == SleepHqSyncState::Working) {
        fail_locked("preempted");
        unlock();
        return JobStep::Idle;
    }
    if (status_.pending && phase_ == WorkPhase::Idle) {
        if (!begin_run_locked(now)) {
            unlock();
            return status_.pending ? JobStep::Waiting : JobStep::Idle;
        }
    }
    if (status_.state != SleepHqSyncState::Working) {
        unlock();
        return JobStep::Idle;
    }

    const WorkPhase phase = phase_;
    if (!phase_has_blocking_io(phase)) {
        const JobStep result = step_work_phase_locked();
        unlock();
        return result;
    }

    const uint32_t operation_generation = operation_generation_.load();
    unlock();

    BlockingResult blocking_result;
    execute_blocking_phase(phase, blocking_result);
    blocking_result.operation_generation = operation_generation;

    if (!lock_ || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) {
        if (blocking_result.local) blocking_result.local.close();
        client_.disconnect();
        Log::logf(CAT_SLEEPHQ,
                  LOG_ERROR,
                  "state publish lock unavailable phase=%u\n",
                  static_cast<unsigned>(phase));
        return JobStep::Idle;
    }
    const JobStep result =
        publish_blocking_phase_locked(phase, blocking_result);
    unlock();
    return result;
}

void SleepHqSyncJob::on_preempt() {
    request_operation_abort();
}

SleepHqSyncStatus SleepHqSyncJob::status() const {
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

SleepHqSyncRuntimeStatus SleepHqSyncJob::runtime_status() const {
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

bool SleepHqSyncJob::active() const {
    return runtime_status().active();
}

}  // namespace aircannect
