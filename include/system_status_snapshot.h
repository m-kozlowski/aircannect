#pragma once

#include <stdint.h>
#include <string>

#include "app_config.h"
#include "as11_device_state.h"
#include "memory_manager.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "storage_manager.h"
#include "time_sync_service.h"
#include "wifi_manager.h"

namespace aircannect {

struct WifiStatusSnapshot {
    std::string state;
    std::string ssid;
    std::string ip;
    std::string bssid;
    SoftApMode softap_mode = SoftApMode::Auto;
    bool softap_running = false;
    bool roaming_enabled = false;
    bool roaming_suspended = false;
    int32_t rssi = 0;
    int32_t channel = 0;
    int8_t active_profile = -1;
};

struct As11StatusSnapshot {
    std::string product_name;
    std::string serial_number;
    std::string software_identifier;
    std::string active_therapy_profile;
    std::string motor_run_meter;
    std::string rop;
    std::string device_datetime;
    As11TherapyState therapy_state = As11TherapyState::Unknown;
    As11TherapyTarget pending_therapy_target = As11TherapyTarget::None;
    bool clock_valid = false;
    uint32_t clock_sample_ms = 0;
};

struct TimeStatusSnapshot {
    std::string esp_time_source;
    std::string esp_datetime;
    bool resmed_time_sync_enabled = false;
    bool ntp_synced = false;
    bool esp_time_valid = false;
};

struct SystemStatusSnapshot {
    uint32_t now_ms = 0;
    uint32_t uptime_s = 0;
    const char *version = "";
    const char *built = "";
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

}  // namespace aircannect
