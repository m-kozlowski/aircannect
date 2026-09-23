#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string>

#include "app_config.h"
#include "as11_clock.h"
#include "as11_device_service.h"
#include "airmini_ncp_clock_port.h"
#include "rpc_request_port.h"
#include "wifi_manager.h"

namespace aircannect {

enum class EspClockSource : uint8_t {
    Unknown,
    Ntp,
    Resmed,
};

class TimeSyncService {
public:
    using ActivityCallback = bool (*)(void *context);

    void initialize_timezone(const AppConfigData &app_config);
    void begin(const AppConfigData &app_config,
               WifiManager &wifi_manager,
               RpcRequestPort &rpc,
               As11DeviceService &device);
    void set_airmini_ncp_clock_port(AirMiniNcpClockPort &port) {
        airmini_clock_ = &port;
    }
    void set_history_transfer_activity_callback(ActivityCallback callback,
                                                void *context);
    void poll();

    void force_ntp_sync();
    bool request_push_esp_to_resmed(RpcSource source);
    bool request_pull_resmed_to_esp(RpcSource source);
    void reset_resmed_push();
    void note_as11_connection_reset();

    bool ntp_synced() const { return ntp_synced_; }
    bool resmed_time_sync_enabled() const {
        return app_config_ && app_config_->resmed_time_sync_enabled;
    }
    bool resmed_time_write_supported() const;
    // History must not share the CAN lane with any NCP clock operation.
    bool clock_write_active() const { return airmini_clock_operation_active_; }
    bool esp_clock_valid() const;
    const char *esp_clock_source_name() const;
    As11ClockTransform as11_clock_transform() const;
    bool refresh_as11_clock_reference();
    uint32_t timezone_revision() const { return timezone_revision_; }
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
    void poll_resmed_push_result(uint32_t now_ms);
    void poll_airmini_clock_result();
    void poll_resmed_push(uint32_t now_ms);
    bool therapy_running() const;
    bool history_transfer_active() const;
    bool airmini_clock_blocked();
    bool request_airmini_clock_write(RpcSource source);
    bool set_esp_time_from_resmed(const std::string &utc_datetime);
    bool format_utc(int64_t epoch_ms, char *out, size_t size) const;

    const AppConfigData *app_config_ = nullptr;
    WifiManager *wifi_manager_ = nullptr;
    RpcRequestPort *rpc_ = nullptr;
    As11DeviceService *device_ = nullptr;
    AirMiniNcpClockPort *airmini_clock_ = nullptr;
    ActivityCallback history_transfer_activity_ = nullptr;
    void *history_transfer_context_ = nullptr;
    bool airmini_clock_operation_active_ = false;
    String applied_timezone_;
    uint32_t timezone_revision_ = 0;

    bool ntp_started_ = false;
    bool ntp_synced_ = false;
    bool ntp_reported_ = false;
    uint32_t ntp_synced_ms_ = 0;

    bool manual_resmed_pull_pending_ = false;
    bool resmed_push_readback_pending_ = false;
    bool resmed_push_readback_awaiting_response_ = false;
    bool resmed_push_available_ = true;

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
