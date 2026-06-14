#include "time_sync_service.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include "as11_rpc.h"
#include "board.h"
#include "calendar_utils.h"
#include "debug_log.h"

namespace aircannect {
namespace {

static constexpr time_t VALID_TIME_MIN_EPOCH = 1609459200;
static constexpr uint32_t TIME_SYNC_RETRY_MS = 30000;
static constexpr uint32_t TIME_SYNC_PULL_RETRY_MS = 15000;
static constexpr uint32_t TIME_SYNC_NTP_FALLBACK_MS = 30000;
static constexpr uint32_t TIME_SYNC_RESMED_PULL_INTERVAL_MS =
    6UL * 60UL * 60UL * 1000UL;
static constexpr uint32_t TIME_SYNC_RESMED_PUSH_INTERVAL_MS =
    6UL * 60UL * 60UL * 1000UL;
static constexpr uint32_t TIME_SYNC_RESMED_PUSH_READBACK_DELAY_MS = 2000;

volatile bool g_ntp_synced = false;

void ntp_sync_cb(struct timeval *) {
    g_ntp_synced = true;
}

bool sta_online(const WifiManager &wifi_manager) {
    const WifiModeState mode = wifi_manager.mode_state();
    return mode == WifiModeState::StaConnected ||
           mode == WifiModeState::StaRoamScanning;
}

time_t utc_fields_to_epoch(int year,
                           int month,
                           int day,
                           int hour,
                           int minute,
                           int second) {
    const int64_t days =
        calendar_days_from_civil(year, static_cast<unsigned>(month),
                                 static_cast<unsigned>(day));
    const int64_t seconds = days * 86400 +
                            static_cast<int64_t>(hour) * 3600 +
                            static_cast<int64_t>(minute) * 60 + second;
    return static_cast<time_t>(seconds);
}

}  // namespace

void TimeSyncService::begin(AppConfig &app_config,
                            WifiManager &wifi_manager,
                            RpcArbiter &arbiter) {
    app_config_ = &app_config;
    wifi_manager_ = &wifi_manager;
    arbiter_ = &arbiter;
    apply_timezone();
    last_status_ = "starting";
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[TIME] NTP enabled, AS11 fallback enabled, AS11 push=%s\n",
              app_config.data().resmed_time_sync_enabled ? "on" : "off");
}

void TimeSyncService::poll() {
    if (!app_config_ || !wifi_manager_ || !arbiter_) return;

    apply_timezone();
    const uint32_t now_ms = millis();
    poll_ntp(now_ms);
    if (g_ntp_synced && !ntp_synced_) note_ntp_sync(now_ms);
    poll_resmed_pull(now_ms);
    poll_resmed_push(now_ms);
}

void TimeSyncService::force_ntp_sync() {
    stop_ntp();
    start_ntp();
    last_status_ = "ntp_sync_requested";
}

bool TimeSyncService::request_push_esp_to_resmed(RpcSource source) {
    if (!arbiter_ || !esp_clock_valid()) {
        last_status_ = "esp_clock_not_valid";
        return false;
    }
    if (therapy_running()) {
        last_status_ = "resmed_push_deferred_therapy_active";
        return false;
    }

    const bool queued = arbiter_->send_set_datetime_now(source);
    if (queued) {
        resmed_push_readback_pending_ = true;
        resmed_push_readback_awaiting_response_ = false;
        next_resmed_push_readback_ms_ =
            millis() + TIME_SYNC_RESMED_PUSH_READBACK_DELAY_MS;
    }
    last_status_ = queued ? "esp_to_resmed_queued" : "esp_to_resmed_queue_full";
    return queued;
}

bool TimeSyncService::request_pull_resmed_to_esp(RpcSource source) {
    if (!arbiter_) return false;
    const bool queued = arbiter_->send_request("GetDateTime", "", source);
    if (queued) {
        if (source != RpcSource::Scheduler) manual_resmed_pull_pending_ = true;
        last_resmed_pull_attempt_ms_ = millis();
        last_status_ = "resmed_to_esp_requested";
    } else {
        last_status_ = "resmed_to_esp_queue_full";
    }
    return queued;
}

void TimeSyncService::reset_resmed_push() {
    next_resmed_push_ms_ = 0;
    last_resmed_push_attempt_ms_ = 0;
    resmed_push_readback_pending_ = false;
    resmed_push_readback_awaiting_response_ = false;
    next_resmed_push_readback_ms_ = 0;
    last_status_ = "resmed_push_reset";
}

bool TimeSyncService::esp_clock_valid() const {
    return time(nullptr) >= VALID_TIME_MIN_EPOCH;
}

const char *TimeSyncService::esp_clock_source_name() const {
    switch (esp_clock_source_) {
        case EspClockSource::Ntp: return "ntp";
        case EspClockSource::Resmed: return "resmed";
        case EspClockSource::Unknown:
        default: return "unknown";
    }
}

std::string TimeSyncService::utc_now_iso() const {
    char out[29];
    if (!utc_now_iso(out, sizeof(out))) return "";
    return std::string(out);
}

bool TimeSyncService::utc_now_iso(char *out, size_t size) const {
    if (!out || size == 0) return false;
    out[0] = 0;
    struct timeval tv = {};
    if (gettimeofday(&tv, nullptr) != 0 ||
        tv.tv_sec < VALID_TIME_MIN_EPOCH) {
        return false;
    }
    const int64_t epoch_ms = static_cast<int64_t>(tv.tv_sec) * 1000 +
                             static_cast<int64_t>(tv.tv_usec / 1000);
    return format_utc(epoch_ms, out, size);
}

void TimeSyncService::apply_timezone() {
    if (!app_config_) return;
    const String &timezone = app_config_->data().timezone;
    if (timezone == applied_timezone_) return;
    if (timezone.length()) {
        setenv("TZ", timezone.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    applied_timezone_ = timezone;
}

void TimeSyncService::start_ntp() {
    apply_timezone();
    g_ntp_synced = false;
    sntp_set_time_sync_notification_cb(ntp_sync_cb);
    if (esp_sntp_enabled()) esp_sntp_stop();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
#if LWIP_DHCP_GET_NTP_SRV
    esp_sntp_servermode_dhcp(true);
#endif
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ntp_started_ = true;
    ntp_started_ms_ = millis();
    ntp_reported_ = false;
    last_status_ = "ntp_started";
    Log::logf(CAT_WIFI, LOG_INFO,
              "[TIME] NTP started: DHCP + pool.ntp.org fallback\n");
}

void TimeSyncService::stop_ntp() {
    if (esp_sntp_enabled()) esp_sntp_stop();
    ntp_started_ = false;
    ntp_started_ms_ = 0;
    ntp_synced_ = false;
    ntp_reported_ = false;
    g_ntp_synced = false;
}

void TimeSyncService::note_ntp_sync(uint32_t now_ms) {
    ntp_synced_ = true;
    esp_clock_source_ = EspClockSource::Ntp;
    last_status_ = "ntp_synced";
    if (app_config_ && app_config_->data().resmed_time_sync_enabled) {
        next_resmed_push_ms_ = now_ms;
    }
    if (ntp_reported_) return;
    ntp_reported_ = true;

    char text[32];
    const std::string utc = utc_now_iso();
    snprintf(text, sizeof(text), "%s", utc.c_str());
    Log::logf(CAT_WIFI, LOG_INFO, "[TIME] NTP synced UTC=%s\n", text);
}

void TimeSyncService::poll_ntp(uint32_t now_ms) {
    if (ntp_synced_) return;
    if (!sta_online(*wifi_manager_)) return;
    if (ntp_started_) return;
    (void)now_ms;
    start_ntp();
}

bool TimeSyncService::resmed_fallback_ready(uint32_t now_ms) const {
    if (ntp_synced_) return false;
    if (!wifi_manager_) return true;
    if (!sta_online(*wifi_manager_)) return true;
    if (!ntp_started_) return false;
    return static_cast<int32_t>(now_ms - ntp_started_ms_) >=
           static_cast<int32_t>(TIME_SYNC_NTP_FALLBACK_MS);
}

void TimeSyncService::poll_resmed_pull(uint32_t now_ms) {
    const As11DeviceState &state = arbiter_->as11_state();
    if (state.clock_valid() &&
        state.clock_sample_ms() != observed_clock_sample_ms_) {
        observed_clock_sample_ms_ = state.clock_sample_ms();
        if (resmed_push_readback_awaiting_response_) {
            resmed_push_readback_awaiting_response_ = false;
            last_status_ = "esp_to_resmed_readback_ok";
        } else if (manual_resmed_pull_pending_ ||
                   resmed_fallback_ready(now_ms)) {
            if (set_esp_time_from_resmed(state.device_datetime())) {
                last_resmed_pull_success_ms_ = now_ms;
                manual_resmed_pull_pending_ = false;
            }
        }
    }

    const bool push_readback_due =
        resmed_push_readback_pending_ &&
        static_cast<int32_t>(now_ms - next_resmed_push_readback_ms_) >= 0;
    const bool fallback_due =
        resmed_fallback_ready(now_ms) &&
        (!last_resmed_pull_success_ms_ ||
         static_cast<int32_t>(now_ms - last_resmed_pull_success_ms_) >=
             static_cast<int32_t>(TIME_SYNC_RESMED_PULL_INTERVAL_MS));
    if (!manual_resmed_pull_pending_ && !fallback_due && !push_readback_due) {
        return;
    }
    if (last_resmed_pull_attempt_ms_ &&
        static_cast<int32_t>(now_ms - last_resmed_pull_attempt_ms_) <
            static_cast<int32_t>(TIME_SYNC_PULL_RETRY_MS)) {
        return;
    }
    last_resmed_pull_attempt_ms_ = now_ms;
    if (request_pull_resmed_to_esp(RpcSource::Scheduler) &&
        push_readback_due) {
        resmed_push_readback_pending_ = false;
        resmed_push_readback_awaiting_response_ = true;
        next_resmed_push_readback_ms_ = 0;
        last_status_ = "esp_to_resmed_readback_queued";
    }
}

void TimeSyncService::poll_resmed_push(uint32_t now_ms) {
    if (!app_config_ || !app_config_->data().resmed_time_sync_enabled) {
        next_resmed_push_ms_ = 0;
        resmed_push_readback_pending_ = false;
        resmed_push_readback_awaiting_response_ = false;
        next_resmed_push_readback_ms_ = 0;
        return;
    }
    if (!ntp_synced_ || !esp_clock_valid()) return;
    if (!next_resmed_push_ms_) next_resmed_push_ms_ = now_ms;
    if (static_cast<int32_t>(now_ms - next_resmed_push_ms_) < 0) return;
    if (therapy_running()) {
        last_status_ = "resmed_push_deferred_therapy_active";
        return;
    }
    if (last_resmed_push_attempt_ms_ &&
        static_cast<int32_t>(now_ms - last_resmed_push_attempt_ms_) <
            static_cast<int32_t>(TIME_SYNC_RETRY_MS)) {
        return;
    }

    last_resmed_push_attempt_ms_ = now_ms;
    if (request_push_esp_to_resmed(RpcSource::Scheduler)) {
        next_resmed_push_ms_ = now_ms + TIME_SYNC_RESMED_PUSH_INTERVAL_MS;
    }
}

bool TimeSyncService::therapy_running() const {
    return arbiter_ &&
           arbiter_->as11_state().therapy_state() ==
               As11TherapyState::Running;
}

bool TimeSyncService::set_esp_time_from_resmed(
    const std::string &utc_datetime) {
    int64_t epoch_ms = 0;
    if (!parse_resmed_datetime_ms(utc_datetime, epoch_ms)) {
        last_status_ = "resmed_time_parse_failed";
        return false;
    }

    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(epoch_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((epoch_ms % 1000) * 1000);
    if (settimeofday(&tv, nullptr) != 0) {
        last_status_ = "settimeofday_failed";
        return false;
    }
    apply_timezone();
    esp_clock_source_ = EspClockSource::Resmed;
    last_status_ = "resmed_to_esp_synced";
    Log::logf(CAT_GENERAL, LOG_INFO, "[TIME] ESP clock set from AS11 UTC=%s\n",
              utc_datetime.c_str());
    return true;
}

bool TimeSyncService::parse_resmed_datetime_ms(
    const std::string &utc_datetime,
    int64_t &epoch_ms) const {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    if (sscanf(utc_datetime.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n",
               &year, &month, &day, &hour, &minute, &second,
               &consumed) != 6) {
        return false;
    }

    int millisecond = 0;
    const char *p = utc_datetime.c_str() + consumed;
    if (*p == '.') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) {
                millisecond = millisecond * 10 + (*p - '0');
            }
            digits++;
            p++;
        }
        if (digits == 0) return false;
        while (digits < 3) {
            millisecond *= 10;
            digits++;
        }
    }
    if (*p != 'Z' || p[1] != 0) return false;

    if (year < 2020 || month < 1 || month > 12 || day < 1 ||
        day > calendar_days_in_month(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }
    const time_t epoch = utc_fields_to_epoch(year, month, day, hour,
                                             minute, second);
    if (epoch < VALID_TIME_MIN_EPOCH) return false;
    epoch_ms = static_cast<int64_t>(epoch) * 1000 + millisecond;
    return true;
}

std::string TimeSyncService::format_utc(int64_t epoch_ms) const {
    char full[29];
    if (!format_utc(epoch_ms, full, sizeof(full))) return "";
    return std::string(full);
}

bool TimeSyncService::format_utc(int64_t epoch_ms,
                                 char *out,
                                 size_t size) const {
    if (!out || size == 0) return false;
    out[0] = 0;
    if (epoch_ms < static_cast<int64_t>(VALID_TIME_MIN_EPOCH) * 1000) {
        return false;
    }
    struct tm utc = {};
    const time_t epoch = static_cast<time_t>(epoch_ms / 1000);
    gmtime_r(&epoch, &utc);
    char base[25];
    const int millisecond = static_cast<int>(epoch_ms % 1000);
    if (strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
        return false;
    }
    const int written = snprintf(out, size, "%s.%03dZ", base, millisecond);
    if (written < 0 || static_cast<size_t>(written) >= size) {
        out[0] = 0;
        return false;
    }
    return true;
}

}  // namespace aircannect
