#include "storage_sync_job.h"

#include <ctype.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "calendar_utils.h"
#include "crc32.h"
#include "debug_log.h"
#include "edf_storage_worker.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t CONFIG_REFRESH_INTERVAL_MS = 1000;
static constexpr size_t SYNC_WALK_MAX_DEPTH = 16;
static constexpr uint32_t SYNC_DATALOG_CUTOFF_DAYS = 90;
static constexpr time_t SYNC_VALID_TIME_MIN_EPOCH = 1609459200;
static constexpr const char *SYNC_METADATA_FILE = "meta.state";
static constexpr const char *SYNC_DATALOG_PREFIX = "/DATALOG/";
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

struct SyncRoot {
    const char *path;
    bool recursive;
};

struct LocalNodeInfo {
    bool exists = false;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

const SyncRoot SYNC_ROOTS[] = {
    {"/DATALOG", true},
    {"/SETTINGS", true},
    {"/STR.edf", false},
    {"/Identification.json", false},
    {"/Identification.crc", false},
    {"/journal.jnl", false},
};

uint32_t millis_nonzero() {
    uint32_t now = millis();
    return now == 0 ? 1 : now;
}

bool all_digits(const char *text, size_t len) {
    if (!text || len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

bool parse_yyyymmdd(const char *text, int &year, unsigned &month,
                    unsigned &day) {
    if (!text || strlen(text) != 8 || !all_digits(text, 8)) return false;
    year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
           (text[2] - '0') * 10 + (text[3] - '0');
    month = (text[4] - '0') * 10 + (text[5] - '0');
    day = (text[6] - '0') * 10 + (text[7] - '0');
    return month >= 1 && month <= 12 && day >= 1 &&
           day <= calendar_days_in_month(year, static_cast<int>(month));
}

bool path_starts_with(const char *path, const char *prefix) {
    if (!path || !prefix) return false;
    const size_t len = strlen(prefix);
    return strncmp(path, prefix, len) == 0;
}

bool append_literal(char *out, size_t out_size, size_t &pos,
                    const char *text) {
    for (const char *p = text ? text : ""; *p; ++p) {
        if (pos + 1 >= out_size) return false;
        out[pos++] = *p;
    }
    out[pos] = '\0';
    return true;
}

bool append_safe_bucket_text(char *out, size_t out_size, size_t &pos,
                             const char *text) {
    bool wrote = false;
    for (const char *p = text ? text : ""; *p; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == '/') {
            if (!wrote) continue;
            if (pos > 0 && out[pos - 1] == '-') continue;
            if (pos + 1 >= out_size) return false;
            out[pos++] = '-';
            wrote = true;
        } else if (isalnum(ch)) {
            if (pos + 1 >= out_size) return false;
            out[pos++] = static_cast<char>(ch);
            wrote = true;
        } else {
            if (!wrote || (pos > 0 && out[pos - 1] == '-')) continue;
            if (pos + 1 >= out_size) return false;
            out[pos++] = '-';
            wrote = true;
        }
    }
    while (pos > 0 && out[pos - 1] == '-') pos--;
    out[pos] = '\0';
    return wrote;
}

bool build_singleton_state_bucket(const char *prefix,
                                  const char *path,
                                  char *out,
                                  size_t out_size) {
    if (!prefix || !path || !out || out_size == 0) return false;
    size_t pos = 0;
    if (!append_literal(out, out_size, pos, prefix)) return false;
    return append_safe_bucket_text(out, out_size, pos, path);
}

bool is_datalog_child_day(const char *name) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    return parse_yyyymmdd(name, year, month, day);
}

void state_hash_update_cstr(uint32_t &crc, const char *text) {
    if (!text) text = "";
    crc = crc32_ieee_update_state(
        crc, reinterpret_cast<const uint8_t *>(text), strlen(text));
}

uint64_t current_epoch_seconds_or_zero() {
    const time_t now = time(nullptr);
    return now >= SYNC_VALID_TIME_MIN_EPOCH ? static_cast<uint64_t>(now) : 0;
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

void read_local_node_info(File &file, LocalNodeInfo &out) {
    out = LocalNodeInfo();
    if (!file) return;
    out.exists = true;
    out.is_dir = file.isDirectory();
    out.size = out.is_dir ? 0 : static_cast<uint64_t>(file.size());
    const time_t last_write = file.getLastWrite();
    out.mtime = last_write > 0 ? static_cast<uint64_t>(last_write) : 0;
}

LocalNodeInfo stat_local_node(const char *path) {
    LocalNodeInfo out;
    if (!path || !path[0]) return out;
    Storage::Guard guard;
    File file = Storage::open(path, "r");
    read_local_node_info(file, out);
    if (file) file.close();
    return out;
}

bool parse_state_line(char *line,
                      uint64_t &size,
                      uint64_t &mtime,
                      const char *&path) {
    if (!line) return false;
    char *first_tab = strchr(line, '\t');
    char *second_tab = first_tab ? strchr(first_tab + 1, '\t') : nullptr;
    if (!first_tab || !second_tab) return false;
    *first_tab = '\0';
    *second_tab = '\0';

    char *end = nullptr;
    const unsigned long long parsed_size = strtoull(line, &end, 10);
    if (!end || *end != '\0') return false;
    end = nullptr;
    const unsigned long long parsed_mtime =
        strtoull(first_tab + 1, &end, 10);
    if (!end || *end != '\0') return false;

    size = static_cast<uint64_t>(parsed_size);
    mtime = static_cast<uint64_t>(parsed_mtime);
    path = second_tab + 1;
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
    out.auto_after_therapy = true;
    out.reconcile_enabled = true;
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

bool StorageSyncJob::reason_is_verify(const char *reason) {
    return reason && (strcmp(reason, SYNC_REASON_STARTUP_CHECK) == 0 ||
                      strcmp(reason, SYNC_REASON_VERIFY_RECENT) == 0);
}

bool StorageSyncJob::reason_is_reconcile(const char *reason) {
    return reason && strcmp(reason, SYNC_REASON_VERIFY_RECENT) == 0;
}

bool StorageSyncJob::config_matches_locked(
    const ConfigSnapshot &config) const {
    return config_.enabled == config.enabled &&
           config_.auto_after_therapy == config.auto_after_therapy &&
           config_.reconcile_enabled == config.reconcile_enabled &&
           strcmp(config_.endpoint, config.endpoint) == 0 &&
           strcmp(config_.user, config.user) == 0 &&
           strcmp(config_.password, config.password) == 0;
}

void StorageSyncJob::reset_run_locked(bool keep_status) {
    close_local_locked();
    close_walk_locked();
    close_latest_verify_locked();
    release_walk_stack_locked();
    release_upload_buffer_locked();
    smb_.abort_connection();
    clear_current_file_locked();
    phase_ = WorkPhase::Idle;
    root_index_ = 0;
    endpoint_hash_ = 0;
    state_dir_[0] = '\0';
    current_run_verify_ = false;
    current_run_reconcile_ = false;
    sync_after_verify_ = false;
    ensured_remote_dir_[0] = '\0';
    latest_datalog_day_[0] = '\0';
    clear_state_cache_locked();
    if (!keep_status) {
        const bool enabled = status_.enabled;
        const bool configured = status_.configured;
        const bool endpoint_set = status_.endpoint_set;
        const bool user_set = status_.user_set;
        const bool password_set = status_.password_set;
        const bool auto_after = status_.auto_after_therapy;
        const bool reconcile = status_.reconcile_enabled;
        const bool network_available = status_.network_available;
        const uint32_t generation = status_.config_generation;
        const uint64_t last_sync_epoch = status_.last_sync_epoch;
        const uint32_t last_sync_files_seen =
            status_.last_sync_files_seen;
        const uint32_t last_sync_files_uploaded =
            status_.last_sync_files_uploaded;
        const uint32_t last_sync_files_skipped =
            status_.last_sync_files_skipped;
        const uint32_t last_sync_files_failed =
            status_.last_sync_files_failed;
        const uint64_t last_sync_bytes_uploaded =
            status_.last_sync_bytes_uploaded;
        const uint64_t last_verify_epoch = status_.last_verify_epoch;
        const uint32_t last_verify_files_seen =
            status_.last_verify_files_seen;
        const uint64_t last_reconcile_epoch =
            status_.last_reconcile_epoch;
        const uint32_t last_reconcile_files_seen =
            status_.last_reconcile_files_seen;
        const uint64_t last_failure_epoch = status_.last_failure_epoch;
        char last_failure_error[AC_STORAGE_ERROR_MAX] = {};
        copy_cstr(last_failure_error, sizeof(last_failure_error),
                  status_.last_failure_error);
        status_ = StorageSyncStatus();
        status_.enabled = enabled;
        status_.configured = configured;
        status_.endpoint_set = endpoint_set;
        status_.user_set = user_set;
        status_.password_set = password_set;
        status_.auto_after_therapy = auto_after;
        status_.reconcile_enabled = reconcile;
        status_.network_available = network_available;
        status_.config_generation = generation;
        status_.last_sync_epoch = last_sync_epoch;
        status_.last_sync_files_seen = last_sync_files_seen;
        status_.last_sync_files_uploaded = last_sync_files_uploaded;
        status_.last_sync_files_skipped = last_sync_files_skipped;
        status_.last_sync_files_failed = last_sync_files_failed;
        status_.last_sync_bytes_uploaded = last_sync_bytes_uploaded;
        status_.last_verify_epoch = last_verify_epoch;
        status_.last_verify_files_seen = last_verify_files_seen;
        status_.last_reconcile_epoch = last_reconcile_epoch;
        status_.last_reconcile_files_seen = last_reconcile_files_seen;
        status_.last_failure_epoch = last_failure_epoch;
        copy_cstr(status_.last_failure_error,
                  sizeof(status_.last_failure_error),
                  last_failure_error);
    }
}

bool StorageSyncJob::build_endpoint_state_dir_locked(
    const ConfigSnapshot &config,
    char *out,
    size_t out_size,
    uint32_t *hash_out) const {
    if (!out || out_size == 0 || !snapshot_configured(config)) return false;
    uint32_t crc = crc32_ieee_initial_state();
    state_hash_update_cstr(crc, config.endpoint);
    state_hash_update_cstr(crc, "\n");
    state_hash_update_cstr(crc, config.user);
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
    status_.auto_after_therapy = config.auto_after_therapy;
    status_.reconcile_enabled = config.reconcile_enabled;
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
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  SYNC_REASON_STARTUP_CHECK);
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
              "[SYNC] config enabled=%u configured=%u auto=%u reconcile=%u\n",
              status_.enabled ? 1u : 0u,
              status_.configured ? 1u : 0u,
              status_.auto_after_therapy ? 1u : 0u,
              status_.reconcile_enabled ? 1u : 0u);
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

bool StorageSyncJob::begin_run_locked(const char *reason) {
    char run_reason[AC_STORAGE_SYNC_REASON_MAX] = {};
    copy_cstr(run_reason, sizeof(run_reason), reason ? reason : "manual");
    const bool verify = reason_is_verify(run_reason);
    const bool reconcile = reason_is_reconcile(run_reason);
    reset_run_locked(false);
    current_run_verify_ = verify;
    current_run_reconcile_ = reconcile;
    retry_due_ms_ = 0;
    status_.state = StorageSyncState::Working;
    status_.pending = true;
    copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
              run_reason);
    status_.last_error[0] = '\0';
    status_.last_run_verify = current_run_verify_;
    status_.last_run_reconcile = current_run_reconcile_;
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
    phase_ = WorkPhase::Connect;
    Log::logf(CAT_STORAGE,
              LOG_INFO,
              "[SYNC] started reason=%s endpoint=%s\n",
              status_.pending_reason[0] ? status_.pending_reason : "manual",
              config_.endpoint);
    return true;
}

bool StorageSyncJob::request_sync_with_reason(const char *reason,
                                              const char *label) {
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
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  reason ? reason : "manual");
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
    return request_sync_with_reason("manual", "manual");
}

bool StorageSyncJob::request_verify_recent() {
    return request_sync_with_reason(SYNC_REASON_VERIFY_RECENT,
                                    "verify_recent");
}

bool StorageSyncJob::request_post_therapy_sync() {
    if (!lock(50)) {
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "[SYNC] post-therapy request rejected reason=lock_timeout\n");
        return false;
    }
    if (!status_.enabled || !status_.configured ||
        !status_.auto_after_therapy) {
        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SYNC] post-therapy request ignored enabled=%u "
                  "configured=%u auto=%u\n",
                  status_.enabled ? 1u : 0u,
                  status_.configured ? 1u : 0u,
                  status_.auto_after_therapy ? 1u : 0u);
        unlock();
        return false;
    }
    if (status_.state != StorageSyncState::Working) {
        status_.pending = true;
        status_.state = StorageSyncState::Pending;
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  "post_therapy");
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

bool StorageSyncJob::ensure_walk_stack_locked() {
    if (walk_stack_) return true;
    walk_capacity_ = SYNC_WALK_MAX_DEPTH;
    walk_stack_ = static_cast<WalkFrame *>(
        Memory::alloc_large(sizeof(WalkFrame) * walk_capacity_, true));
    if (!walk_stack_) {
        fail_locked("walk_alloc");
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[SYNC] walk stack allocation failed bytes=%u\n",
                  static_cast<unsigned>(sizeof(WalkFrame) * walk_capacity_));
        return false;
    }
    for (size_t i = 0; i < walk_capacity_; ++i) {
        new (&walk_stack_[i]) WalkFrame();
    }
    return true;
}

void StorageSyncJob::close_walk_locked() {
    if (!walk_stack_) return;
    for (size_t i = 0; i < walk_depth_; ++i) {
        if (walk_stack_[i].opened) {
            Storage::Guard guard;
            walk_stack_[i].dir.close();
            walk_stack_[i].opened = false;
        }
    }
}

void StorageSyncJob::release_walk_stack_locked() {
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

bool StorageSyncJob::push_dir_locked(const char *path) {
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

bool StorageSyncJob::ensure_dir_open_locked(WalkFrame &frame) {
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
    for (uint32_t i = 0; i < frame.next_index; ++i) {
        File skipped = frame.dir.openNextFile();
        if (!skipped) {
            frame.dir.close();
            fail_locked("walk_resume_failed");
            return false;
        }
        skipped.close();
    }
    frame.opened = true;
    return true;
}

bool StorageSyncJob::datalog_day_allowed(const char *name) const {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!parse_yyyymmdd(name, year, month, day)) return false;
    const time_t now = time(nullptr);
    if (now < 1700000000) return true;
    const int64_t today_days = static_cast<int64_t>(now / 86400);
    const int64_t dir_days = calendar_days_from_civil(year, month, day);
    return dir_days >= today_days -
        static_cast<int64_t>(SYNC_DATALOG_CUTOFF_DAYS);
}

bool StorageSyncJob::latest_datalog_day_locked(char *out,
                                               size_t out_size,
                                               char *error_out,
                                               size_t error_out_size) const {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    File dir;
    LocalNodeInfo dir_info;
    {
        Storage::Guard guard;
        dir = Storage::open("/DATALOG", "r");
        read_local_node_info(dir, dir_info);
    }
    if (!dir_info.exists) return true;
    if (!dir_info.is_dir) {
        Storage::Guard guard;
        dir.close();
        return true;
    }

    char best[9] = {};
    for (;;) {
        char name[AC_STORAGE_PATH_MAX] = {};
        bool have_child = false;
        LocalNodeInfo info;
        {
            Storage::Guard guard;
            File child = dir.openNextFile();
            if (child) {
                have_child = true;
                copy_cstr(name, sizeof(name),
                          storage_basename_from_path(child.name()));
                read_local_node_info(child, info);
                child.close();
            }
        }
        if (!have_child) break;
        if (info.is_dir && is_datalog_child_day(name) &&
            (best[0] == '\0' || strcmp(name, best) > 0)) {
            copy_cstr(best, sizeof(best), name);
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

    char name[AC_STORAGE_PATH_MAX] = {};
    bool have_child = false;
    LocalNodeInfo info;
    {
        Storage::Guard guard;
        File child = latest_verify_.dir.openNextFile();
        if (child) {
            have_child = true;
            copy_cstr(name, sizeof(name),
                      storage_basename_from_path(child.name()));
            read_local_node_info(child, info);
            child.close();
        }
    }
    if (!have_child) {
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
    if (info.is_dir) return true;

    char child_path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_append_child_path(latest_verify_.day_path,
                                   name,
                                   child_path,
                                   sizeof(child_path)) ||
        !storage_user_path_valid(child_path)) {
        copy_cstr(error_out, error_out_size, "bad_child_path");
        return false;
    }

    status_.files_seen++;
    status_.updated_ms = millis_nonzero();
    if (!state_contains_locked(latest_verify_.state_path,
                               child_path,
                               info.size,
                               info.mtime)) {
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
    if (!remote.exists || remote.directory || remote.size != info.size) {
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
                  static_cast<unsigned long long>(info.size));
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

bool StorageSyncJob::root_step_locked() {
    while (root_index_ < sizeof(SYNC_ROOTS) / sizeof(SYNC_ROOTS[0])) {
        const SyncRoot &root = SYNC_ROOTS[root_index_++];
        const LocalNodeInfo info = stat_local_node(root.path);
        if (!info.exists) continue;
        if (info.is_dir) {
            if (root.recursive && push_dir_locked(root.path)) return true;
            if (status_.state == StorageSyncState::Error) return false;
            continue;
        }
        return plan_file_locked(root.path);
    }
    phase_ = WorkPhase::Finish;
    return true;
}

bool StorageSyncJob::walk_step_locked() {
    if (walk_depth_ == 0) return root_step_locked();
    WalkFrame &frame = walk_stack_[walk_depth_ - 1];
    if (!ensure_dir_open_locked(frame)) return false;

    char name[AC_STORAGE_PATH_MAX] = {};
    bool have_child = false;
    LocalNodeInfo info;
    {
        Storage::Guard guard;
        File child = frame.dir.openNextFile();
        if (child) {
            have_child = true;
            copy_cstr(name, sizeof(name),
                      storage_basename_from_path(child.name()));
            read_local_node_info(child, info);
            child.close();
        }
    }

    if (!have_child) {
        maybe_mark_completed_datalog_day_locked(frame.path);
        {
            Storage::Guard guard;
            frame.dir.close();
            frame.opened = false;
        }
        walk_depth_--;
        return true;
    }
    frame.next_index++;

    char child_path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_append_child_path(frame.path,
                                   name,
                                   child_path,
                                   sizeof(child_path)) ||
        !storage_user_path_valid(child_path)) {
        fail_locked("bad_child_path");
        return false;
    }
    if (strcmp(frame.path, "/DATALOG") == 0) {
        if (!info.is_dir || !datalog_day_allowed(name)) return true;
        if (!current_run_reconcile_ &&
            datalog_day_is_finalized_locked(name) &&
            datalog_day_done_locked(name)) {
            return true;
        }
    }
    if (info.is_dir) return push_dir_locked(child_path);
    return plan_file_locked(child_path);
}

bool StorageSyncJob::next_file_locked() {
    for (;;) {
        if (!Storage::mounted()) {
            fail_locked("storage_unavailable");
            return false;
        }
        if (!walk_step_locked()) return false;
        if (phase_ != WorkPhase::NextFile) return true;
        return true;
    }
}

bool StorageSyncJob::build_state_path_locked(const char *path,
                                             char *out,
                                             size_t out_size,
                                             StateWriteMode *write_mode) const {
    if (!path || !out || out_size == 0 || !state_dir_[0]) return false;
    StateWriteMode mode = StateWriteMode::Replace;
    char bucket[AC_STORAGE_NAME_MAX] = {};
    if (path_starts_with(path, SYNC_DATALOG_PREFIX)) {
        const char *day = path + strlen(SYNC_DATALOG_PREFIX);
        if (all_digits(day, 8) && (day[8] == '/' || day[8] == '\0')) {
            memcpy(bucket, day, 8);
            bucket[8] = '\0';
        } else {
            copy_cstr(bucket, sizeof(bucket), "datalog");
        }
        mode = StateWriteMode::Append;
    } else if (path_starts_with(path, "/SETTINGS/") ||
               strcmp(path, "/SETTINGS") == 0) {
        const char *settings_path = strcmp(path, "/SETTINGS") == 0
            ? "root"
            : path + strlen("/SETTINGS/");
        if (!build_singleton_state_bucket("settings-",
                                          settings_path,
                                          bucket,
                                          sizeof(bucket))) {
            return false;
        }
    } else {
        const char *root_path = path[0] == '/' ? path + 1 : path;
        if (!build_singleton_state_bucket("root-",
                                          root_path,
                                          bucket,
                                          sizeof(bucket))) {
            return false;
        }
    }
    const int written = snprintf(out, out_size, "%s/%s.state",
                                 state_dir_, bucket);
    if (write_mode) *write_mode = mode;
    return written > 0 && static_cast<size_t>(written) < out_size;
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

void StorageSyncJob::clear_state_cache_locked() {
    if (state_cache_.entries) Memory::free(state_cache_.entries);
    state_cache_ = StateCache();
}

bool StorageSyncJob::reserve_state_cache_locked(size_t needed) {
    if (needed <= state_cache_.capacity) return true;
    size_t next = state_cache_.capacity == 0 ? 8 : state_cache_.capacity * 2;
    while (next < needed) next *= 2;
    StateCacheEntry *entries = static_cast<StateCacheEntry *>(
        Memory::alloc_large(sizeof(StateCacheEntry) * next, true));
    if (!entries) {
        Log::logf(CAT_STORAGE,
                  LOG_ERROR,
                  "[SYNC] state cache allocation failed entries=%u bytes=%u\n",
                  static_cast<unsigned>(next),
                  static_cast<unsigned>(sizeof(StateCacheEntry) * next));
        return false;
    }
    for (size_t i = 0; i < next; ++i) new (&entries[i]) StateCacheEntry();
    for (size_t i = 0; i < state_cache_.count; ++i) {
        entries[i] = state_cache_.entries[i];
    }
    if (state_cache_.entries) Memory::free(state_cache_.entries);
    state_cache_.entries = entries;
    state_cache_.capacity = next;
    return true;
}

bool StorageSyncJob::add_state_cache_entry_locked(uint64_t size,
                                                  uint64_t mtime,
                                                  const char *path) {
    if (!path) return false;
    for (size_t i = 0; i < state_cache_.count; ++i) {
        StateCacheEntry &entry = state_cache_.entries[i];
        if (strcmp(entry.path, path) == 0) {
            entry.size = size;
            entry.mtime = mtime;
            return true;
        }
    }
    if (!reserve_state_cache_locked(state_cache_.count + 1)) return false;
    StateCacheEntry &entry = state_cache_.entries[state_cache_.count++];
    entry.size = size;
    entry.mtime = mtime;
    copy_cstr(entry.path, sizeof(entry.path), path);
    return true;
}

bool StorageSyncJob::load_state_cache_locked(const char *state_path) {
    if (!state_path || !state_path[0]) return false;
    if (state_cache_.loaded &&
        strcmp(state_cache_.path, state_path) == 0) {
        return true;
    }
    clear_state_cache_locked();
    copy_cstr(state_cache_.path, sizeof(state_cache_.path), state_path);
    state_cache_.loaded = true;

    File file;
    {
        Storage::Guard guard;
        file = Storage::open(state_path, "r");
    }
    if (!file) return true;

    uint8_t buffer[512] = {};
    char line[AC_STORAGE_PATH_MAX + 64] = {};
    size_t line_len = 0;
    bool ok = true;
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
                uint64_t size = 0;
                uint64_t mtime = 0;
                const char *path = nullptr;
                if (parse_state_line(line, size, mtime, path) &&
                    !add_state_cache_entry_locked(size, mtime, path)) {
                    ok = false;
                    break;
                }
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            } else {
                line_len = 0;
            }
        }
        if (!ok) break;
    }
    if (ok && line_len > 0) {
        line[line_len] = '\0';
        uint64_t size = 0;
        uint64_t mtime = 0;
        const char *path = nullptr;
        if (parse_state_line(line, size, mtime, path) &&
            !add_state_cache_entry_locked(size, mtime, path)) {
            ok = false;
        }
    }
    {
        Storage::Guard guard;
        file.close();
    }
    if (!ok) {
        clear_state_cache_locked();
        return false;
    }
    return true;
}

bool StorageSyncJob::state_contains_locked(const char *state_path,
                                           const char *path,
                                           uint64_t size,
                                           uint64_t mtime) {
    if (!state_path || !path) return false;
    if (!load_state_cache_locked(state_path)) return false;
    for (size_t i = 0; i < state_cache_.count; ++i) {
        const StateCacheEntry &entry = state_cache_.entries[i];
        if (entry.size == size &&
            entry.mtime == mtime &&
            strcmp(entry.path, path) == 0) {
            return true;
        }
    }
    return false;
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
    if (!state_cache_.loaded ||
        !state_path ||
        strcmp(state_cache_.path, state_path) != 0) {
        return;
    }
    if (mode == StateWriteMode::Replace) {
        state_cache_.count = 0;
    }
    (void)add_state_cache_entry_locked(size, mtime, path);
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
    if (!path || !out || out_size < 9) return false;
    if (!path_starts_with(path, SYNC_DATALOG_PREFIX)) return false;
    const char *day = path + strlen(SYNC_DATALOG_PREFIX);
    if (strlen(day) != 8 || !all_digits(day, 8)) return false;
    memcpy(out, day, 8);
    out[8] = '\0';
    return true;
}

bool StorageSyncJob::datalog_day_done_path_locked(const char *day,
                                                  char *out,
                                                  size_t out_size) const {
    if (!day || strlen(day) != 8 || !all_digits(day, 8) || !state_dir_[0] ||
        !out || out_size == 0) {
        return false;
    }
    const int written = snprintf(out, out_size, "%s/%s.done",
                                 state_dir_, day);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool StorageSyncJob::datalog_day_done_locked(const char *day) const {
    char path[AC_STORAGE_SYNC_STATE_PATH_MAX] = {};
    if (!datalog_day_done_path_locked(day, path, sizeof(path))) return false;
    const LocalNodeInfo info = stat_local_node(path);
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
    if (!day || strlen(day) != 8 || !all_digits(day, 8) ||
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
    const char *path) {
    if (current_run_reconcile_) return;
    char day[9] = {};
    if (!datalog_day_name_from_path(path, day, sizeof(day))) return;
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

bool StorageSyncJob::plan_file_locked(const char *path) {
    clear_current_file_locked();
    copy_cstr(current_file_.path, sizeof(current_file_.path), path);
    copy_cstr(status_.current_path, sizeof(status_.current_path), path);
    status_.files_seen++;
    status_.updated_ms = millis_nonzero();

    const LocalNodeInfo info = stat_local_node(path);
    if (!info.exists || info.is_dir) {
        phase_ = WorkPhase::NextFile;
        return true;
    }
    current_file_.size = info.size;
    current_file_.mtime = info.mtime;

    if (!build_state_path_locked(path,
                                 current_file_.state_path,
                                 sizeof(current_file_.state_path),
                                 &current_file_.state_write_mode)) {
        fail_locked("state_path_failed");
        return false;
    }
    const bool local_state_complete =
        state_contains_locked(current_file_.state_path,
                              path,
                              info.size,
                              info.mtime);
    if (local_state_complete) {
        if (!current_run_reconcile_) {
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
    if (current_run_reconcile_) {
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
            remote.size == info.size) {
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
                  static_cast<unsigned long long>(info.size));
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
    release_walk_stack_locked();
    release_upload_buffer_locked();
    clear_state_cache_locked();
    clear_current_file_locked();
    ensured_remote_dir_[0] = '\0';
    const bool queue_sync =
        current_run_verify_ && !current_run_reconcile_ && sync_after_verify_;
    status_.state = queue_sync ? StorageSyncState::Pending
                               : StorageSyncState::Idle;
    status_.pending = queue_sync;
    if (queue_sync) {
        copy_cstr(status_.pending_reason, sizeof(status_.pending_reason),
                  SYNC_REASON_STARTUP_SYNC);
    } else {
        status_.pending_reason[0] = '\0';
    }
    status_.last_error[0] = '\0';
    status_.updated_ms = millis_nonzero();
    retry_due_ms_ = 0;
    retry_attempt_ = 0;
    phase_ = WorkPhase::Idle;
    if (current_run_verify_) {
        status_.last_verify_epoch = current_epoch_seconds_or_zero();
        status_.last_verify_files_seen = status_.files_seen;
        if (current_run_reconcile_) {
            status_.last_reconcile_epoch = status_.last_verify_epoch;
            status_.last_reconcile_files_seen = status_.files_seen;
        }
        if (current_run_reconcile_ && status_.files_uploaded > 0) {
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
        status_.last_sync_epoch = current_epoch_seconds_or_zero();
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
    current_run_verify_ = false;
    current_run_reconcile_ = false;
    sync_after_verify_ = false;
}

void StorageSyncJob::fail_locked(const char *error) {
    const WorkPhase failed_phase = phase_;
    const bool failed_verify_only =
        current_run_verify_ && !current_run_reconcile_;
    close_local_locked();
    close_walk_locked();
    close_latest_verify_locked();
    smb_.abort_connection();
    release_upload_buffer_locked();
    clear_state_cache_locked();
    ensured_remote_dir_[0] = '\0';
    status_.state = StorageSyncState::Error;
    status_.pending = false;
    if (!failed_verify_only) status_.files_failed++;
    copy_cstr(status_.last_error, sizeof(status_.last_error),
              error ? error : "sync_error");
    status_.last_failure_epoch = current_epoch_seconds_or_zero();
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
    current_run_verify_ = false;
    current_run_reconcile_ = false;
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

JobStep StorageSyncJob::step() {
    if (!lock(20)) return JobStep::Waiting;
    const uint32_t now_ms = millis_nonzero();
    const uint32_t defer_until = idle_defer_until_ms_.load();
    if (defer_until != 0 &&
        static_cast<int32_t>(now_ms - defer_until) < 0 &&
        status_.state != StorageSyncState::Working) {
        unlock();
        return status_.pending ? JobStep::Waiting : JobStep::Idle;
    }
    if (status_.state == StorageSyncState::Error && retry_due_ms_ != 0 &&
        static_cast<int32_t>(now_ms - retry_due_ms_) >= 0) {
        status_.pending = true;
        status_.state = StorageSyncState::Pending;
        if (!status_.pending_reason[0]) {
            copy_cstr(status_.pending_reason,
                      sizeof(status_.pending_reason),
                      status_.last_run_verify ? SYNC_REASON_STARTUP_CHECK :
                                                 "retry");
        }
        status_.updated_ms = now_ms;
    }
    if (status_.state == StorageSyncState::Idle &&
        status_.enabled &&
        status_.configured &&
        status_.reconcile_enabled &&
        network_available_.load()) {
        const uint64_t now_epoch = current_epoch_seconds_or_zero();
        const bool due =
            now_epoch != 0 &&
            (status_.last_reconcile_epoch == 0 ||
             now_epoch >= status_.last_reconcile_epoch +
                 SYNC_RECONCILE_INTERVAL_SECONDS);
        if (due) {
            status_.pending = true;
            status_.state = StorageSyncState::Pending;
            copy_cstr(status_.pending_reason,
                      sizeof(status_.pending_reason),
                      SYNC_REASON_VERIFY_RECENT);
            status_.updated_ms = millis_nonzero();
            Log::logf(CAT_STORAGE,
                      LOG_INFO,
                      "[SYNC] queued reason=%s last_reconcile=%llu\n",
                      status_.pending_reason,
                      static_cast<unsigned long long>(
                          status_.last_reconcile_epoch));
        }
    }
    const bool ready =
        status_.enabled && status_.configured &&
        (status_.pending || status_.state == StorageSyncState::Working);
    if (!ready) {
        unlock();
        return JobStep::Idle;
    }
    if (status_.pending && status_.state != StorageSyncState::Working &&
        !network_available_.load()) {
        status_.network_available = false;
        unlock();
        return JobStep::Idle;
    }
    status_.network_available = network_available_.load();
    const EdfStorageWorkerStatus edf = EdfStorageWorker::status();
    if (edf.busy || edf.queued > 0 || edf.open_file_count > 0) {
        unlock();
        return JobStep::Waiting;
    }
    if (status_.state != StorageSyncState::Working &&
        !begin_run_locked(status_.pending_reason)) {
        unlock();
        return JobStep::Idle;
    }

    char error[AC_STORAGE_ERROR_MAX] = {};
    JobStep result = JobStep::Working;
    switch (phase_) {
        case WorkPhase::Idle:
            phase_ = WorkPhase::Connect;
            break;

        case WorkPhase::Connect:
            if (!smb_.configure(config_.endpoint, config_.user,
                                config_.password, error, sizeof(error)) ||
                !smb_.connect(error, sizeof(error))) {
                fail_locked(error[0] ? error : "smb_connect_failed");
                result = JobStep::Idle;
                break;
            }
            if (current_run_verify_ && !current_run_reconcile_) {
                if (!verify_endpoint_base_locked(error, sizeof(error))) {
                    fail_locked(error[0] ? error : "endpoint_verify_failed");
                    result = JobStep::Idle;
                    break;
                }
                phase_ = WorkPhase::VerifyLatestStart;
                break;
            }
            if (!prepare_upload_buffer_locked()) {
                fail_locked("upload_buffer_alloc");
                result = JobStep::Idle;
                break;
            }
            phase_ = WorkPhase::NextFile;
            break;

        case WorkPhase::VerifyLatestStart:
            if (!begin_latest_verify_locked(error, sizeof(error))) {
                fail_locked(error[0] ? error : "latest_verify_start_failed");
                result = JobStep::Idle;
            }
            break;

        case WorkPhase::VerifyLatestFile:
            if (!latest_verify_file_step_locked(error, sizeof(error))) {
                fail_locked(error[0] ? error : "latest_verify_failed");
                result = JobStep::Idle;
            }
            break;

        case WorkPhase::VerifyLatestInvalidate:
            if (!invalidate_latest_state_locked(error, sizeof(error))) {
                fail_locked(error[0] ? error : "state_invalidate_failed");
                result = JobStep::Idle;
            }
            break;

        case WorkPhase::NextFile:
            if (!next_file_locked()) result = JobStep::Idle;
            break;

        case WorkPhase::EnsureRemoteDir:
            if (strcmp(ensured_remote_dir_, current_file_.remote_dir) != 0) {
                if (!smb_.ensure_directory(current_file_.remote_dir,
                                           error, sizeof(error))) {
                    fail_locked(error[0] ? error : "remote_mkdir_failed");
                    result = JobStep::Idle;
                    break;
                }
                copy_cstr(ensured_remote_dir_,
                          sizeof(ensured_remote_dir_),
                          current_file_.remote_dir);
            }
            phase_ = WorkPhase::OpenLocal;
            break;

        case WorkPhase::OpenLocal: {
            Storage::Guard guard;
            current_file_.local = Storage::open(current_file_.path, "r");
            if (!current_file_.local || current_file_.local.isDirectory()) {
                fail_locked("local_open_failed");
                result = JobStep::Idle;
                break;
            }
            current_file_.local_open = true;
            phase_ = WorkPhase::OpenRemote;
            break;
        }

        case WorkPhase::OpenRemote:
            if (!smb_.open_writer(current_file_.remote_path,
                                  error, sizeof(error))) {
                fail_locked(error[0] ? error : "remote_open_failed");
                result = JobStep::Idle;
                break;
            }
            current_file_.offset = 0;
            phase_ = current_file_.size == 0 ? WorkPhase::CloseRemote
                                             : WorkPhase::UploadChunk;
            break;

        case WorkPhase::UploadChunk: {
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
                result = JobStep::Idle;
                break;
            }
            const int written =
                smb_.write(upload_buffer_, read, error, sizeof(error));
            if (written < 0 || static_cast<size_t>(written) != read) {
                fail_locked(error[0] ? error : "remote_write_failed");
                result = JobStep::Idle;
                break;
            }
            current_file_.offset += static_cast<uint64_t>(written);
            status_.bytes_uploaded += static_cast<uint64_t>(written);
            status_.updated_ms = millis_nonzero();
            if (current_file_.offset >= current_file_.size) {
                phase_ = WorkPhase::CloseRemote;
            }
            break;
        }

        case WorkPhase::CloseRemote:
            if (!smb_.close_writer(error, sizeof(error))) {
                fail_locked(error[0] ? error : "remote_close_failed");
                result = JobStep::Idle;
                break;
            }
            close_local_locked();
            phase_ = WorkPhase::MarkState;
            break;

        case WorkPhase::MarkState: {
            const LocalNodeInfo info = stat_local_node(current_file_.path);
            if (!info.exists || info.is_dir ||
                info.size != current_file_.size ||
                info.mtime != current_file_.mtime) {
                fail_locked("local_changed");
                result = JobStep::Idle;
                break;
            }
            if (!write_state_locked(current_file_.state_path,
                                    current_file_.path,
                                    current_file_.size,
                                    current_file_.mtime,
                                    current_file_.state_write_mode)) {
                if (status_.state != StorageSyncState::Error) {
                    fail_locked("state_write_failed");
                }
                result = JobStep::Idle;
                break;
            }
            status_.files_uploaded++;
            clear_current_file_locked();
            phase_ = WorkPhase::NextFile;
            break;
        }

        case WorkPhase::Finish:
            finish_run_locked();
            result = JobStep::Idle;
            break;
    }

    unlock();
    return result;
}

void StorageSyncJob::on_preempt() {
    if (!lock(5)) return;
    if (status_.state == StorageSyncState::Working) {
        char reason[AC_STORAGE_SYNC_REASON_MAX] = {};
        copy_cstr(reason, sizeof(reason), status_.pending_reason);
        reset_run_locked(true);
        status_.state = StorageSyncState::Pending;
        status_.pending = true;
        status_.updated_ms = millis_nonzero();
        copy_cstr(status_.pending_reason,
                  sizeof(status_.pending_reason),
                  reason[0] ? reason : "preempt");
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
