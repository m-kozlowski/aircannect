#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string_view>

#include "app_config.h"
#include "as11_device_state.h"
#include "board.h"
#include "memory_manager.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "storage_manager.h"
#include "time_sync_service.h"
#include "wifi_manager.h"

namespace aircannect {

static constexpr size_t AC_STATUS_IP_TEXT_MAX = 46;
static constexpr size_t AC_STATUS_ISO_TIME_TEXT_MAX = 29;

struct WifiStatusSnapshot {
    std::string_view state;
    std::string_view ssid;
    char ip[AC_STATUS_IP_TEXT_MAX] = "";
    char bssid[AC_WIFI_BSSID_TEXT_MAX] = "";
    SoftApMode softap_mode = SoftApMode::Auto;
    bool softap_running = false;
    bool roaming_enabled = false;
    bool roaming_suspended = false;
    int32_t rssi = 0;
    int32_t channel = 0;
    int8_t active_profile = -1;
};

struct As11StatusSnapshot {
    std::string_view product_name;
    std::string_view serial_number;
    std::string_view software_identifier;
    std::string_view active_therapy_profile;
    std::string_view motor_run_meter;
    std::string_view rop;
    std::string_view device_datetime;
    As11TherapyState therapy_state = As11TherapyState::Unknown;
    As11TherapyTarget pending_therapy_target = As11TherapyTarget::None;
    bool clock_valid = false;
    uint32_t clock_sample_ms = 0;
};

struct TimeStatusSnapshot {
    std::string_view esp_time_source;
    char esp_datetime[AC_STATUS_ISO_TIME_TEXT_MAX] = "";
    bool resmed_time_sync_enabled = false;
    bool ntp_synced = false;
    bool esp_time_valid = false;
};

struct SystemStatusSnapshot {
    uint32_t now_ms = 0;
    uint32_t uptime_s = 0;
    const char *version = "";
    const char *built = "";
    const char *reset_reason = "";
    MemoryStatus memory;
    StorageStatus storage;
    WifiStatusSnapshot wifi;
    bool ota_active = false;
    As11StatusSnapshot as11;
    OximetryRuntimeStatus oximetry;
    TimeStatusSnapshot time;
};

struct SystemStatusSources {
    const RpcArbiter &arbiter;
    const WifiManager &wifi_manager;
    const AppConfig &app_config;
    const TimeSyncService &time_sync_service;
    const OtaManager &ota_manager;
    const OximetryManager &oximetry_manager;
};

SystemStatusSnapshot collect_system_status(
    const SystemStatusSources &sources);

const char *system_reset_reason_name();

}  // namespace aircannect
