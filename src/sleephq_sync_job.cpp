#include "sleephq_sync_job.h"

#include <ctype.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <esp_rom_md5.h>

#include "crc32.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_directory.h"
#include "storage_export_plan.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t CONFIG_REFRESH_INTERVAL_MS = 1000;
static constexpr size_t SLEEPHQ_WALK_MAX_DEPTH = 16;
static constexpr uint32_t SLEEPHQ_DATALOG_SCAN_BUDGET = 16;
static constexpr uint32_t SLEEPHQ_IMPORT_POLL_INTERVAL_MS = 2000;
static constexpr uint32_t SLEEPHQ_IMPORT_POLL_TIMEOUT_MS = 120000;
static constexpr uint32_t SLEEPHQ_RETRY_BACKOFF_MS[] = {
    15UL * 60UL * 1000UL,
    60UL * 60UL * 1000UL,
    6UL * 60UL * 60UL * 1000UL,
};
static constexpr const char *SLEEPHQ_INFLIGHT_FILE = "inflight.state";

const char *const SLEEPHQ_ROOT_METADATA_FILES[] = {
    "/STR.edf",
    "/Identification.json",
    "/Identification.crc",
};

void sleep_hq_digest_to_hex(const uint8_t digest[16],
                            char out[AC_SLEEPHQ_CONTENT_HASH_MAX]) {
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = HEX_DIGITS[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0F];
    }
    out[32] = '\0';
}

enum class ImportStatusKind : uint8_t {
    Success,
    Transient,
    Failure,
    Unknown,
};

uint32_t millis_nonzero() {
    const uint32_t now = millis();
    return now == 0 ? 1 : now;
}

bool status_is_one_of(const char *status, const char *const *values,
                      size_t count) {
    if (!status || !status[0]) return false;
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(status, values[i]) == 0) return true;
    }
    return false;
}

bool status_contains_failure_token(const char *status) {
    if (!status || !status[0]) return false;
    char lower[AC_SLEEPHQ_STATUS_MAX] = {};
    copy_cstr(lower, sizeof(lower), status);
    for (char *p = lower; *p; ++p) {
        *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    }
    return strstr(lower, "fail") || strstr(lower, "error") ||
           strstr(lower, "reject") || strstr(lower, "cancel");
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

ImportStatusKind classify_import_status(const char *status) {
    static const char *const SUCCESS[] = {
        "processed",
        "complete",
        "completed",
        "finished",
        "success",
        "succeeded",
    };
    static const char *const TRANSIENT[] = {
        "created",
        "new",
        "pending",
        "queued",
        "ready",
        "uploading",
        "uploaded",
        "unpacking",
        "processing",
        "importing",
    };
    static const char *const FAILURE[] = {
        "failed",
        "failure",
        "error",
        "errored",
        "rejected",
        "cancelled",
        "canceled",
    };

    if (status_is_one_of(status, SUCCESS,
                         sizeof(SUCCESS) / sizeof(SUCCESS[0]))) {
        return ImportStatusKind::Success;
    }
    if (status_is_one_of(status, TRANSIENT,
                         sizeof(TRANSIENT) / sizeof(TRANSIENT[0]))) {
        return ImportStatusKind::Transient;
    }
    if (status_is_one_of(status, FAILURE,
                         sizeof(FAILURE) / sizeof(FAILURE[0])) ||
        status_contains_failure_token(status)) {
        return ImportStatusKind::Failure;
    }
    return ImportStatusKind::Unknown;
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
    return strcmp(config_.client_id, config.client_id) == 0 &&
           strcmp(config_.client_secret, config.client_secret) == 0 &&
           strcmp(config_.team_id, config.team_id) == 0 &&
           strcmp(config_.device_id, config.device_id) == 0;
}

void SleepHqSyncJob::apply_config_locked(const ConfigSnapshot &config) {
    if (config_matches_locked(config)) return;
    if (status_.state == SleepHqSyncState::Working) {
        abort_requested_.store(true);
        return;
    }
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
}

void SleepHqSyncJob::refresh_config(const AppConfigData &config,
                                    uint32_t now_ms) {
    if (last_config_check_ms_ != 0 &&
        static_cast<uint32_t>(now_ms - last_config_check_ms_) <
            CONFIG_REFRESH_INTERVAL_MS) {
        return;
    }
    last_config_check_ms_ = now_ms == 0 ? 1 : now_ms;
    const ConfigSnapshot snapshot = make_config_snapshot(config);
    if (!lock()) return;
    apply_config_locked(snapshot);
    unlock();
}

void SleepHqSyncJob::set_network_available(bool available) {
    network_available_.store(available);
    if (!lock(5)) return;
    status_.network_available = available;
    unlock();
    if (available) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
}

void SleepHqSyncJob::set_runtime_blocked(bool blocked) {
    runtime_blocked_.store(blocked);
    if (blocked) abort_requested_.store(true);
}

bool SleepHqSyncJob::request_locked(RunKind kind, const char *reason) {
    if (!status_.configured) {
        status_.state = SleepHqSyncState::Disabled;
        return false;
    }
    if (status_.state == SleepHqSyncState::Working) return false;
    pending_run_kind_ = kind;
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
    if (!lock()) return false;
    const bool queued = request_locked(RunKind::Check, reason);
    unlock();
    if (queued) {
        if (BackgroundWorker *worker = background_worker()) worker->wake();
    }
    return queued;
}

bool SleepHqSyncJob::request_sync(const char *reason) {
    if (!lock()) return false;
    const bool queued = request_locked(RunKind::Sync, reason);
    unlock();
    if (queued) {
        Log::logf(CAT_SLEEPHQ, LOG_INFO, "sync queued reason=%s\n",
                  reason && *reason ? reason : "manual");
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
        return false;
    }
    if (!network_available_.load()) {
        status_.network_available = false;
        return false;
    }
    if (runtime_blocked_.load()) return false;

    const RunKind kind = pending_run_kind_;
    char reason[AC_SLEEPHQ_SYNC_REASON_MAX] = {};
    copy_cstr(reason, sizeof(reason),
              status_.pending_reason[0] ? status_.pending_reason : "manual");

    reset_run_locked(false);
    current_run_kind_ = kind;
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
    Log::logf(CAT_SLEEPHQ, LOG_INFO, "started reason=%s mode=%s\n",
              status_.pending_reason[0] ? status_.pending_reason : "manual",
              current_run_kind_ == RunKind::Check ? "check" : "sync");
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

void SleepHqSyncJob::close_walk_locked() {
    if (!walk_stack_) return;
    for (size_t i = 0; i < walk_depth_; ++i) {
        if (walk_stack_[i].opened) {
            Storage::Guard guard;
            walk_stack_[i].dir.close();
            walk_stack_[i].opened = false;
        }
    }
}

void SleepHqSyncJob::release_walk_stack_locked() {
    close_walk_locked();
    if (walk_stack_) {
        for (size_t i = 0; i < walk_capacity_; ++i) {
            walk_stack_[i].~WalkFrame();
        }
        Memory::free(walk_stack_);
    }
    walk_stack_ = nullptr;
    walk_capacity_ = 0;
    walk_depth_ = 0;
}

void SleepHqSyncJob::close_datalog_scan_locked() {
    if (!datalog_scan_opened_) return;
    Storage::Guard guard;
    datalog_scan_dir_.close();
    datalog_scan_opened_ = false;
}

void SleepHqSyncJob::clear_datalog_days_locked() {
    close_datalog_scan_locked();
    if (datalog_days_) {
        for (size_t i = 0; i < datalog_day_capacity_; ++i) {
            datalog_days_[i].~DatalogDay();
        }
        Memory::free(datalog_days_);
    }
    datalog_days_ = nullptr;
    datalog_day_count_ = 0;
    datalog_day_capacity_ = 0;
    datalog_day_index_ = 0;
    datalog_scan_complete_ = false;
    datalog_scan_next_index_ = 0;
    day_batch_active_ = false;
    day_root_index_ = 0;
    day_batch_path_[0] = '\0';
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

void SleepHqSyncJob::reset_run_locked(bool keep_status) {
    client_.disconnect();
    close_local_locked();
    close_walk_locked();
    release_walk_stack_locked();
    clear_datalog_days_locked();
    clear_current_file_locked();
    clear_staged_locked();
    state_cache_.clear();
    phase_ = WorkPhase::Idle;
    day_root_index_ = 0;
    import_process_started_ms_ = 0;
    import_poll_due_ms_ = 0;
    inflight_phase_ = InflightPhase::None;
    state_dir_[0] = 0;
    current_run_kind_ = RunKind::Check;
    abort_requested_.store(false);
    if (!keep_status) {
        const bool configured = status_.configured;
        const bool network_available = status_.network_available;
        const uint32_t generation = status_.config_generation;
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
}

void SleepHqSyncJob::finish_sync_locked() {
    remove_inflight_locked();
    status_.last_sync_epoch = storage_export_current_epoch_seconds_or_zero();
    status_.last_sync_files_seen = status_.files_seen;
    status_.last_sync_files_uploaded = status_.files_uploaded;
    status_.last_sync_files_failed = status_.files_failed;
    status_.last_sync_bytes_uploaded = status_.bytes_uploaded;
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
}

void SleepHqSyncJob::fail_locked(const char *error) {
    if (error && strcmp(error, "preempted") == 0) {
        const RunKind kind = current_run_kind_;
        char reason[AC_SLEEPHQ_SYNC_REASON_MAX] = {};
        copy_cstr(reason, sizeof(reason),
                  status_.pending_reason[0]
                      ? status_.pending_reason
                      : "preempted");
        reset_run_locked(true);
        pending_run_kind_ = kind;
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
        return;
    }

    copy_cstr(status_.last_error, sizeof(status_.last_error),
              error && *error ? error : "failed");
    status_.last_failure_epoch = storage_export_current_epoch_seconds_or_zero();
    status_.state = SleepHqSyncState::Error;
    status_.pending = false;
    status_.updated_ms = millis_nonzero();
    status_.files_failed++;
    const bool retryable =
        status_.configured && sleep_hq_error_retryable(status_.last_error);
    if (retryable) {
        const size_t backoff_count =
            sizeof(SLEEPHQ_RETRY_BACKOFF_MS) /
            sizeof(SLEEPHQ_RETRY_BACKOFF_MS[0]);
        const size_t index = retry_attempt_ < backoff_count
            ? retry_attempt_
            : backoff_count - 1;
        retry_due_ms_ = status_.updated_ms + SLEEPHQ_RETRY_BACKOFF_MS[index];
        if (retry_attempt_ < 255) retry_attempt_++;
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
}

bool SleepHqSyncJob::ensure_walk_stack_locked() {
    if (walk_stack_) return true;
    walk_capacity_ = SLEEPHQ_WALK_MAX_DEPTH;
    walk_stack_ = static_cast<WalkFrame *>(
        Memory::alloc_large(sizeof(WalkFrame) * walk_capacity_, false));
    if (!walk_stack_) {
        fail_locked("walk_alloc");
        Log::logf(CAT_SLEEPHQ, LOG_ERROR,
                  "walk stack allocation failed bytes=%u\n",
                  static_cast<unsigned>(sizeof(WalkFrame) * walk_capacity_));
        return false;
    }
    for (size_t i = 0; i < walk_capacity_; ++i) {
        new (&walk_stack_[i]) WalkFrame();
    }
    return true;
}

bool SleepHqSyncJob::push_dir_locked(const char *path) {
    if (!ensure_walk_stack_locked()) return false;
    if (walk_depth_ >= walk_capacity_) {
        fail_locked("max_depth");
        return false;
    }
    WalkFrame &frame = walk_stack_[walk_depth_++];
    frame = WalkFrame();
    copy_cstr(frame.path, sizeof(frame.path), path);
    return true;
}

bool SleepHqSyncJob::ensure_dir_open_locked(WalkFrame &frame) {
    if (frame.opened) return true;
    Storage::Guard guard;
    frame.dir = Storage::open(frame.path, "r");
    if (!frame.dir) {
        fail_locked("local_not_found");
        return false;
    }
    if (!frame.dir.isDirectory()) {
        frame.dir.close();
        fail_locked("local_not_directory");
        return false;
    }
    if (!storage_skip_dir_children(frame.dir, frame.next_index)) {
        frame.dir.close();
        fail_locked("walk_resume_failed");
        return false;
    }
    frame.opened = true;
    return true;
}

bool SleepHqSyncJob::reserve_datalog_days_locked(size_t needed) {
    if (needed <= datalog_day_capacity_) return true;
    size_t next = datalog_day_capacity_ == 0 ? 16 : datalog_day_capacity_ * 2;
    while (next < needed) next *= 2;
    DatalogDay *items = static_cast<DatalogDay *>(
        Memory::alloc_large(sizeof(DatalogDay) * next, false));
    if (!items) {
        Log::logf(CAT_SLEEPHQ, LOG_ERROR,
                  "DATALOG day allocation failed entries=%u bytes=%u\n",
                  static_cast<unsigned>(next),
                  static_cast<unsigned>(sizeof(DatalogDay) * next));
        fail_locked("datalog_day_alloc");
        return false;
    }
    for (size_t i = 0; i < next; ++i) new (&items[i]) DatalogDay();
    for (size_t i = 0; i < datalog_day_count_; ++i) {
        items[i] = datalog_days_[i];
    }
    if (datalog_days_) Memory::free(datalog_days_);
    datalog_days_ = items;
    datalog_day_capacity_ = next;
    return true;
}

bool SleepHqSyncJob::add_datalog_day_locked(const char *day) {
    if (!day || !storage_export_is_datalog_day_name(day)) return true;
    for (size_t i = 0; i < datalog_day_count_; ++i) {
        if (strcmp(datalog_days_[i].day, day) == 0) return true;
    }
    if (!reserve_datalog_days_locked(datalog_day_count_ + 1)) return false;

    size_t pos = 0;
    while (pos < datalog_day_count_ &&
           strcmp(datalog_days_[pos].day, day) > 0) {
        pos++;
    }
    for (size_t i = datalog_day_count_; i > pos; --i) {
        datalog_days_[i] = datalog_days_[i - 1];
    }
    DatalogDay &entry = datalog_days_[pos];
    copy_cstr(entry.day, sizeof(entry.day), day);
    const int written = snprintf(entry.path, sizeof(entry.path),
                                 "/DATALOG/%s", day);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(entry.path)) {
        fail_locked("datalog_day_path");
        return false;
    }
    datalog_day_count_++;
    return true;
}

bool SleepHqSyncJob::scan_datalog_days_step_locked() {
    if (datalog_scan_complete_) return true;
    if (!datalog_scan_opened_) {
        Storage::Guard guard;
        datalog_scan_dir_ = Storage::open("/DATALOG", "r");
        if (!datalog_scan_dir_) {
            datalog_scan_complete_ = true;
            return true;
        }
        if (!datalog_scan_dir_.isDirectory()) {
            datalog_scan_dir_.close();
            datalog_scan_complete_ = true;
            return true;
        }
        if (!storage_skip_dir_children(datalog_scan_dir_,
                                       datalog_scan_next_index_)) {
            datalog_scan_dir_.close();
            fail_locked("datalog_scan_resume_failed");
            return false;
        }
        datalog_scan_opened_ = true;
    }

    for (uint32_t i = 0; i < SLEEPHQ_DATALOG_SCAN_BUDGET; ++i) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(datalog_scan_dir_, child)) {
            close_datalog_scan_locked();
            datalog_scan_complete_ = true;
            datalog_day_index_ = 0;
            return true;
        }
        datalog_scan_next_index_++;
        if (!child.is_dir ||
            !storage_export_datalog_day_allowed(child.name)) {
            continue;
        }
        if (!add_datalog_day_locked(child.name)) return false;
    }
    return true;
}

bool SleepHqSyncJob::datalog_day_has_pending_files_locked(
    const DatalogDay &day) {
    if (!day.path[0]) return false;
    File dir;
    {
        Storage::Guard guard;
        dir = Storage::open(day.path, "r");
    }
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            Storage::Guard guard;
            dir.close();
        }
        return false;
    }

    bool pending = false;
    for (;;) {
        StorageDirChild child;
        if (!storage_read_next_dir_child(dir, child)) break;
        if (child.is_dir) continue;

        char child_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(day.path,
                                       child.name,
                                       child_path,
                                       sizeof(child_path)) ||
            !storage_user_path_valid(child_path)) {
            continue;
        }
        char state_path[AC_SLEEPHQ_SYNC_STATE_PATH_MAX] = {};
        StateWriteMode mode = StateWriteMode::Append;
        if (!build_state_path_locked(child_path,
                                     state_path,
                                     sizeof(state_path),
                                     &mode)) {
            continue;
        }
        const uint64_t mtime =
            child.last_write > 0 ? static_cast<uint64_t>(child.last_write) : 0;
        if (!state_contains_locked(state_path, child_path, child.size, mtime)) {
            pending = true;
            break;
        }
    }
    {
        Storage::Guard guard;
        dir.close();
    }
    return pending;
}

bool SleepHqSyncJob::select_next_datalog_day_locked() {
    while (datalog_day_index_ < datalog_day_count_) {
        const DatalogDay &day = datalog_days_[datalog_day_index_++];
        if (!datalog_day_has_pending_files_locked(day)) continue;
        copy_cstr(day_batch_path_, sizeof(day_batch_path_), day.path);
        day_batch_active_ = true;
        day_root_index_ = 0;
        return true;
    }
    return false;
}

void SleepHqSyncJob::reset_import_batch_locked() {
    remove_inflight_locked();
    close_local_locked();
    close_walk_locked();
    release_walk_stack_locked();
    clear_current_file_locked();
    clear_staged_locked();
    status_.import_id = 0;
    status_.import_status[0] = '\0';
    import_process_started_ms_ = 0;
    import_poll_due_ms_ = 0;
    mark_index_ = 0;
    day_batch_active_ = false;
    day_root_index_ = 0;
    day_batch_path_[0] = '\0';
    phase_ = WorkPhase::NextFile;
}

JobStep SleepHqSyncJob::finish_import_or_sync_locked() {
    if (current_run_kind_ == RunKind::Sync && day_batch_active_) {
        reset_import_batch_locked();
        return JobStep::Working;
    }
    finish_sync_locked();
    return JobStep::Idle;
}

bool SleepHqSyncJob::root_step_locked() {
    if (!datalog_scan_complete_) {
        return scan_datalog_days_step_locked();
    }
    if (!day_batch_active_ && !select_next_datalog_day_locked()) {
        phase_ = WorkPhase::Finish;
        return true;
    }

    const size_t root_count = sizeof(SLEEPHQ_ROOT_METADATA_FILES) /
                              sizeof(SLEEPHQ_ROOT_METADATA_FILES[0]);
    while (day_root_index_ < root_count) {
        const char *path = SLEEPHQ_ROOT_METADATA_FILES[day_root_index_++];
        const StorageLocalNodeInfo info = storage_stat_local_node(path);
        if (!info.exists) continue;
        if (info.is_dir) continue;
        return plan_file_locked(path, true);
    }

    if (!day_batch_path_[0]) {
        phase_ = staged_count_ == 0 ? WorkPhase::Finish
                                    : WorkPhase::ProcessImport;
        return true;
    }
    if (push_dir_locked(day_batch_path_)) return true;
    if (status_.state == SleepHqSyncState::Error) return false;
    phase_ = staged_count_ == 0 ? WorkPhase::Finish
                                : WorkPhase::ProcessImport;
    return true;
}

bool SleepHqSyncJob::walk_step_locked() {
    if (walk_depth_ == 0) return root_step_locked();
    WalkFrame &frame = walk_stack_[walk_depth_ - 1];
    if (!ensure_dir_open_locked(frame)) return false;

    StorageDirChild child;
    if (!storage_read_next_dir_child(frame.dir, child)) {
        {
            Storage::Guard guard;
            frame.dir.close();
            frame.opened = false;
        }
        walk_depth_--;
        if (walk_depth_ == 0 && day_batch_active_) {
            phase_ = staged_count_ == 0 ? WorkPhase::Finish
                                        : WorkPhase::ProcessImport;
        }
        return true;
    }
    frame.next_index++;

    char child_path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_append_child_path(frame.path,
                                   child.name,
                                   child_path,
                                   sizeof(child_path)) ||
        !storage_user_path_valid(child_path)) {
        fail_locked("bad_child_path");
        return false;
    }
    if (child.is_dir) return push_dir_locked(child_path);
    return plan_file_locked(child_path);
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
    if (!walk_step_locked()) return false;
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

bool SleepHqSyncJob::compute_current_file_content_hash_locked(char *out,
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

bool SleepHqSyncJob::build_state_path_locked(const char *path,
                                             char *out,
                                             size_t out_size,
                                             StateWriteMode *mode_out) const {
    if (!path || !out || out_size == 0 || !state_dir_[0]) return false;
    StorageExportStateWriteMode export_mode =
        StorageExportStateWriteMode::Replace;
    const bool ok = storage_export_build_state_path(state_dir_,
                                                    path,
                                                    out,
                                                    out_size,
                                                    &export_mode);
    if (mode_out) {
        *mode_out =
            export_mode == StorageExportStateWriteMode::Append
                ? StateWriteMode::Append
                : StateWriteMode::Replace;
    }
    return ok;
}

bool SleepHqSyncJob::plan_file_locked(const char *path, bool force_upload) {
    clear_current_file_locked();
    copy_cstr(current_file_.path, sizeof(current_file_.path), path);
    copy_cstr(status_.current_path, sizeof(status_.current_path), path);
    current_file_.force_upload = force_upload;
    status_.files_seen++;
    status_.updated_ms = millis_nonzero();

    const StorageLocalNodeInfo info = storage_stat_local_node(path);
    if (!info.exists || info.is_dir) {
        phase_ = WorkPhase::NextFile;
        return true;
    }
    current_file_.size = info.size;
    current_file_.mtime = info.mtime;
    if (!build_sleep_path_locked(path,
                                 current_file_.sleep_path,
                                 sizeof(current_file_.sleep_path),
                                 current_file_.name,
                                 sizeof(current_file_.name))) {
        fail_locked("sleep_path_failed");
        return false;
    }
    if (!build_state_path_locked(path,
                                 current_file_.state_path,
                                 sizeof(current_file_.state_path),
                                 &current_file_.state_write_mode)) {
        fail_locked("state_path_failed");
        return false;
    }
    const bool state_has_file =
        state_contains_locked(current_file_.state_path,
                              path,
                              current_file_.size,
                              current_file_.mtime);
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
    return true;
}

bool SleepHqSyncJob::local_ensure_dir_locked(const char *path) {
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

bool SleepHqSyncJob::ensure_state_dir_locked() {
    return state_dir_[0] && local_ensure_dir_locked(state_dir_);
}

bool SleepHqSyncJob::state_contains_locked(const char *state_path,
                                           const char *path,
                                           uint64_t size,
                                           uint64_t mtime) {
    return state_cache_.contains(state_path, path, size, mtime);
}

void SleepHqSyncJob::note_state_written_locked(const char *state_path,
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

bool SleepHqSyncJob::append_state_locked(const StagedFile &file_info) {
    if (!ensure_state_dir_locked()) return false;
    File file;
    {
        Storage::Guard guard;
        file = Storage::open(file_info.state_path, "a");
        if (!file) return false;
        file.printf("%llu\t%llu\t%s\t%lu\t%s\n",
                    static_cast<unsigned long long>(file_info.size),
                    static_cast<unsigned long long>(file_info.mtime),
                    file_info.content_hash,
                    static_cast<unsigned long>(file_info.import_id),
                    file_info.path);
        file.close();
    }
    return true;
}

bool SleepHqSyncJob::replace_state_locked(const StagedFile &file_info) {
    if (!ensure_state_dir_locked()) return false;
    File file;
    {
        Storage::Guard guard;
        file = Storage::open(file_info.state_path, "w");
        if (!file) return false;
        const size_t written =
            file.printf("%llu\t%llu\t%s\t%lu\t%s\n",
                        static_cast<unsigned long long>(file_info.size),
                        static_cast<unsigned long long>(file_info.mtime),
                        file_info.content_hash,
                        static_cast<unsigned long>(file_info.import_id),
                        file_info.path);
        file.close();
        if (written == 0) return false;
    }
    return true;
}

bool SleepHqSyncJob::write_state_locked(const StagedFile &file_info) {
    const bool ok = file_info.state_write_mode == StateWriteMode::Replace
        ? replace_state_locked(file_info)
        : append_state_locked(file_info);
    if (ok) {
        note_state_written_locked(file_info.state_path,
                                  file_info.path,
                                  file_info.size,
                                  file_info.mtime,
                                  file_info.state_write_mode);
    }
    return ok;
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

JobStep SleepHqSyncJob::step_connect_locked(char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    client_.configure(client_config_from_snapshot(config_));
    uint32_t team_id = 0;
    if (!client_.resolve_team_id(team_id)) {
        fail_locked(client_.last_error());
        return JobStep::Idle;
    }
    status_.team_id = team_id;
    if (current_run_kind_ == RunKind::Check) {
        finish_check_locked(team_id);
        return JobStep::Idle;
    }
    if (!build_endpoint_state_dir_locked(team_id,
                                         state_dir_,
                                         sizeof(state_dir_))) {
        fail_locked("state_path_failed");
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

JobStep SleepHqSyncJob::step_check_locked(char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    phase_ = WorkPhase::Connect;
    return JobStep::Working;
}

JobStep SleepHqSyncJob::step_create_import_locked(char *error,
                                                  size_t error_size) {
    SleepHqImportInfo import;
    if (!client_.create_import(status_.team_id, import)) {
        fail_locked(client_.last_error());
        return JobStep::Idle;
    }
    if (!import.id) {
        copy_cstr(error, error_size, "import_id_missing");
        fail_locked(error);
        return JobStep::Idle;
    }
    status_.import_id = import.id;
    if (!write_inflight_locked(InflightPhase::Uploading)) {
        fail_locked("inflight_write_failed");
        return JobStep::Idle;
    }
    phase_ = WorkPhase::OpenLocal;
    return JobStep::Working;
}

JobStep SleepHqSyncJob::step_open_local_locked() {
    Storage::Guard guard;
    current_file_.local = Storage::open(current_file_.path, "r");
    if (!current_file_.local || current_file_.local.isDirectory()) {
        fail_locked("local_open_failed");
        return JobStep::Idle;
    }
    current_file_.local_open = true;
    current_file_.offset = 0;
    phase_ = WorkPhase::UploadFile;
    return JobStep::Working;
}

bool SleepHqSyncJob::upload_read_cb(void *ctx,
                                    uint8_t *out,
                                    size_t len,
                                    size_t &read) {
    SleepHqSyncJob *job = static_cast<SleepHqSyncJob *>(ctx);
    read = 0;
    if (!job || !out || job->abort_requested_.load()) return false;
    Storage::Guard guard;
    read = job->current_file_.local.read(out, len);
    job->current_file_.offset += static_cast<uint64_t>(read);
    return read == len;
}

bool SleepHqSyncJob::upload_reset_cb(void *ctx) {
    SleepHqSyncJob *job = static_cast<SleepHqSyncJob *>(ctx);
    if (!job || !job->current_file_.local_open) return false;
    Storage::Guard guard;
    const bool ok = job->current_file_.local.seek(0);
    if (ok) job->current_file_.offset = 0;
    return ok;
}

bool SleepHqSyncJob::upload_abort_cb(void *ctx) {
    SleepHqSyncJob *job = static_cast<SleepHqSyncJob *>(ctx);
    return !job || job->abort_requested_.load() ||
           job->runtime_blocked_.load();
}

JobStep SleepHqSyncJob::step_upload_file_locked(char *error,
                                                size_t error_size) {
    SleepHqUploadResult upload;
    bool attached = false;
    if (current_file_.attach_by_hash) {
        if (!compute_current_file_content_hash_locked(
                current_file_.content_hash,
                sizeof(current_file_.content_hash))) {
            fail_locked("hash_failed");
            return JobStep::Idle;
        }
        SleepHqAttachRequest attach;
        attach.import_id = status_.import_id;
        attach.name = current_file_.name;
        attach.path = current_file_.sleep_path;
        attach.content_hash = current_file_.content_hash;
        if (client_.attach_file(attach, upload)) {
            attached = true;
        } else if (!upload_reset_cb(this)) {
            copy_cstr(error, error_size, client_.last_error());
            fail_locked(error[0] ? error : "attach_failed");
            return JobStep::Idle;
        }
    }
    if (!attached) {
        SleepHqUploadRequest request;
        request.import_id = status_.import_id;
        request.name = current_file_.name;
        request.path = current_file_.sleep_path;
        request.size = current_file_.size;
        request.read = &SleepHqSyncJob::upload_read_cb;
        request.reset = &SleepHqSyncJob::upload_reset_cb;
        request.should_abort = &SleepHqSyncJob::upload_abort_cb;
        request.ctx = this;
        if (!client_.upload_file(request, upload)) {
            copy_cstr(error, error_size, client_.last_error());
            fail_locked(error[0] ? error : "upload_failed");
            return JobStep::Idle;
        }
    }
    close_local_locked();
    const StorageLocalNodeInfo info = storage_stat_local_node(current_file_.path);
    if (!info.exists || info.is_dir ||
        info.size != current_file_.size ||
        info.mtime != current_file_.mtime) {
        fail_locked("local_changed");
        return JobStep::Idle;
    }
    if (!add_staged_locked(upload)) {
        fail_locked("staged_alloc");
        return JobStep::Idle;
    }
    if (!write_inflight_locked(InflightPhase::Uploading)) {
        fail_locked("inflight_write_failed");
        return JobStep::Idle;
    }
    status_.bytes_uploaded += upload.bytes;
    status_.files_uploaded++;
    clear_current_file_locked();
    phase_ = WorkPhase::NextFile;
    return JobStep::Working;
}

JobStep SleepHqSyncJob::step_process_import_locked(char *error,
                                                   size_t error_size) {
    if (staged_count_ == 0) {
        phase_ = WorkPhase::Finish;
        return JobStep::Working;
    }
    if (!client_.process_import(status_.import_id, nullptr)) {
        copy_cstr(error, error_size, client_.last_error());
        fail_locked(error[0] ? error : "process_import_failed");
        return JobStep::Idle;
    }
    if (!write_inflight_locked(InflightPhase::Processing)) {
        fail_locked("inflight_write_failed");
        return JobStep::Idle;
    }
    import_process_started_ms_ = millis_nonzero();
    import_poll_due_ms_ = import_process_started_ms_;
    status_.import_status[0] = 0;
    phase_ = WorkPhase::WaitImport;
    return JobStep::Waiting;
}

JobStep SleepHqSyncJob::step_wait_import_locked(char *error,
                                                size_t error_size) {
    if (staged_count_ == 0) {
        phase_ = WorkPhase::Finish;
        return JobStep::Working;
    }

    const uint32_t now = millis_nonzero();
    if (import_poll_due_ms_ != 0 &&
        static_cast<int32_t>(now - import_poll_due_ms_) < 0) {
        return JobStep::Waiting;
    }

    SleepHqImportInfo import;
    if (!client_.get_import(status_.import_id, import)) {
        copy_cstr(error, error_size, client_.last_error());
        if (strcmp(error, "http_404") == 0) {
            remove_inflight_locked();
        }
        fail_locked(error[0] ? error : "import_status_failed");
        return JobStep::Idle;
    }
    char previous_status[AC_SLEEPHQ_STATUS_MAX] = {};
    copy_cstr(previous_status, sizeof(previous_status),
              status_.import_status);
    copy_cstr(status_.import_status, sizeof(status_.import_status),
              import.status);
    if (import.id) status_.import_id = import.id;
    status_.updated_ms = now;

    switch (classify_import_status(import.status)) {
        case ImportStatusKind::Success:
            mark_index_ = 0;
            phase_ = WorkPhase::MarkState;
            return JobStep::Working;
        case ImportStatusKind::Failure:
            copy_cstr(error, error_size,
                      import.failed_reason[0] ? import.failed_reason
                                               : "import_failed");
            remove_inflight_locked();
            fail_locked(error);
            return JobStep::Idle;
        case ImportStatusKind::Unknown:
            if (strcmp(previous_status, import.status) != 0) {
                Log::logf(CAT_SLEEPHQ, LOG_WARN,
                          "unknown import status treated as transient: %s\n",
                          import.status[0] ? import.status : "<empty>");
            }
            break;
        case ImportStatusKind::Transient:
            break;
    }

    if (import_process_started_ms_ == 0 ||
        static_cast<uint32_t>(now - import_process_started_ms_) >=
            SLEEPHQ_IMPORT_POLL_TIMEOUT_MS) {
        fail_locked("import_process_timeout");
        return JobStep::Idle;
    }
    import_poll_due_ms_ = now + SLEEPHQ_IMPORT_POLL_INTERVAL_MS;
    return JobStep::Waiting;
}

JobStep SleepHqSyncJob::step_mark_state_locked(char *error,
                                               size_t error_size) {
    if (mark_index_ >= staged_count_) {
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

JobStep SleepHqSyncJob::step_work_phase_locked() {
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    switch (phase_) {
        case WorkPhase::Idle:
            phase_ = WorkPhase::Connect;
            return JobStep::Working;
        case WorkPhase::Connect:
            return step_connect_locked(error, sizeof(error));
        case WorkPhase::Check:
            return step_check_locked(error, sizeof(error));
        case WorkPhase::NextFile:
            return next_file_locked() ? JobStep::Working : JobStep::Idle;
        case WorkPhase::CreateImport:
            return step_create_import_locked(error, sizeof(error));
        case WorkPhase::OpenLocal:
            return step_open_local_locked();
        case WorkPhase::UploadFile:
            return step_upload_file_locked(error, sizeof(error));
        case WorkPhase::ProcessImport:
            return step_process_import_locked(error, sizeof(error));
        case WorkPhase::WaitImport:
            return step_wait_import_locked(error, sizeof(error));
        case WorkPhase::MarkState:
            return step_mark_state_locked(error, sizeof(error));
        case WorkPhase::Finish:
            return finish_import_or_sync_locked();
    }
    return JobStep::Idle;
}

JobStep SleepHqSyncJob::step() {
    if (!lock(20)) return JobStep::Waiting;
    const uint32_t now = millis_nonzero();
    queue_retry_locked(now);
    if (runtime_blocked_.load() && status_.state == SleepHqSyncState::Working) {
        abort_requested_.store(true);
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
    const JobStep result = step_work_phase_locked();
    unlock();
    return result;
}

void SleepHqSyncJob::on_preempt() {
    abort_requested_.store(true);
    if (!lock(5)) return;
    if (status_.state == SleepHqSyncState::Working) {
        const RunKind kind = current_run_kind_;
        char reason[AC_SLEEPHQ_SYNC_REASON_MAX] = {};
        copy_cstr(reason, sizeof(reason),
                  status_.pending_reason[0]
                      ? status_.pending_reason
                      : "preempted");
        reset_run_locked(true);
        pending_run_kind_ = kind;
        status_.state = SleepHqSyncState::Pending;
        status_.pending = true;
        copy_cstr(status_.pending_reason,
                  sizeof(status_.pending_reason),
                  reason);
        status_.updated_ms = millis_nonzero();
        retry_due_ms_ = 0;
    }
    unlock();
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
        out = status_;
        out.retry_due_ms = retry_due_ms_;
        out.retry_attempt = retry_attempt_;
        out.network_available = network_available_.load();
        out.state = SleepHqSyncState::Working;
    }
    return out;
}

bool SleepHqSyncJob::active() const {
    if (!lock(20)) return true;
    const bool out =
        status_.state == SleepHqSyncState::Working ||
        ((status_.state == SleepHqSyncState::Pending || status_.pending) &&
         network_available_.load());
    unlock();
    return out;
}

}  // namespace aircannect
