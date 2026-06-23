#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string>

#include "app_config.h"
#include "rpc_arbiter.h"
#include "wifi_manager.h"

namespace aircannect {

enum class EspClockSource : uint8_t {
    Unknown,
    Ntp,
    Resmed,
};

class TimeSyncService {
public:
    void begin(AppConfig &app_config,
               WifiManager &wifi_manager,
               RpcArbiter &arbiter);
    void poll();

    void force_ntp_sync();
    bool request_push_esp_to_resmed(RpcSource source);
    bool request_pull_resmed_to_esp(RpcSource source);
    void reset_resmed_push();

    bool ntp_synced() const { return ntp_synced_; }
    bool resmed_time_sync_enabled() const {
        return app_config_ && app_config_->data().resmed_time_sync_enabled;
    }
    bool esp_clock_valid() const;
    EspClockSource esp_clock_source() const { return esp_clock_source_; }
    const char *esp_clock_source_name() const;
    const char *last_status() const { return last_status_.c_str(); }
    bool utc_now_iso(char *out, size_t size) const;
    std::string utc_now_iso() const;

private:
    void apply_timezone();
    void start_ntp();
    void stop_ntp();
    void note_ntp_sync(uint32_t now_ms);
    void poll_ntp(uint32_t now_ms);

    bool resmed_fallback_ready(uint32_t now_ms) const;
    bool resmed_pull_due(uint32_t now_ms) const;
    void poll_resmed_pull(uint32_t now_ms);
    void poll_resmed_push(uint32_t now_ms);
    bool therapy_running() const;
    bool set_esp_time_from_resmed(const std::string &utc_datetime);
    bool parse_resmed_datetime_ms(const std::string &utc_datetime,
                                  int64_t &epoch_ms) const;
    bool format_utc(int64_t epoch_ms, char *out, size_t size) const;
    std::string format_utc(int64_t epoch_ms) const;

    AppConfig *app_config_ = nullptr;
    WifiManager *wifi_manager_ = nullptr;
    RpcArbiter *arbiter_ = nullptr;
    String applied_timezone_;

    bool ntp_started_ = false;
    bool ntp_synced_ = false;
    bool ntp_reported_ = false;

    bool manual_resmed_pull_pending_ = false;
    bool resmed_push_readback_pending_ = false;
    bool resmed_push_readback_awaiting_response_ = false;

    uint32_t ntp_started_ms_ = 0;
    uint32_t last_resmed_push_attempt_ms_ = 0;
    uint32_t next_resmed_push_ms_ = 0;
    uint32_t next_resmed_push_readback_ms_ = 0;
    uint32_t last_resmed_pull_attempt_ms_ = 0;
    uint32_t last_resmed_pull_success_ms_ = 0;
    uint32_t observed_clock_sample_ms_ = 0;

    EspClockSource esp_clock_source_ = EspClockSource::Unknown;
    std::string last_status_ = "idle";
};

}  // namespace aircannect
