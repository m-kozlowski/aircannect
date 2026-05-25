#include "system_status_snapshot.h"

#include <stdio.h>

#include "version.h"

namespace aircannect {

SystemStatusSnapshot collect_system_status(
    const SystemStatusSources &sources) {
    SystemStatusSnapshot out;
    out.now_ms = millis();
    out.uptime_s = out.now_ms / 1000;
    out.version = aircannect_version();
    out.built = aircannect_build_date();

    out.memory = Memory::status();
    out.storage = Storage::status();

    out.wifi.state = sources.wifi_manager.state_name();
    out.wifi.ssid = sources.wifi_manager.sta_ssid().c_str();
    const IPAddress ip = sources.wifi_manager.ip();
    snprintf(out.wifi.ip, sizeof(out.wifi.ip), "%u.%u.%u.%u",
             static_cast<unsigned>(ip[0]),
             static_cast<unsigned>(ip[1]),
             static_cast<unsigned>(ip[2]),
             static_cast<unsigned>(ip[3]));
    sources.wifi_manager.bssid(out.wifi.bssid, sizeof(out.wifi.bssid));
    out.wifi.softap_mode = sources.wifi_manager.softap_mode();
    out.wifi.softap_running = sources.wifi_manager.softap_running();
    out.wifi.roaming_enabled = sources.wifi_manager.roaming_enabled();
    out.wifi.roaming_suspended = sources.wifi_manager.roaming_suspended();
    out.wifi.rssi = sources.wifi_manager.rssi();
    out.wifi.channel = sources.wifi_manager.channel();
    out.wifi.active_profile =
        sources.wifi_manager.active_profile_index();

    out.ota_active = sources.ota_manager.active();

    const As11DeviceState &as11 = sources.arbiter.as11_state();
    out.as11.product_name = as11.product_name();
    out.as11.serial_number = as11.serial_number();
    out.as11.software_identifier = as11.software_identifier();
    out.as11.active_therapy_profile = as11.active_therapy_profile();
    out.as11.motor_run_meter = as11.mhr();
    out.as11.rop = as11.rop();
    out.as11.device_datetime = as11.device_datetime();
    out.as11.therapy_state = as11.therapy_state();
    out.as11.pending_therapy_target = as11.pending_therapy_target();
    out.as11.clock_valid = as11.clock_valid();
    out.as11.clock_sample_ms = as11.clock_sample_ms();

    out.oximetry = sources.oximetry_manager.runtime_status();

    out.time.resmed_time_sync_enabled =
        sources.app_config.data().resmed_time_sync_enabled;
    out.time.ntp_synced = sources.time_sync_service.ntp_synced();
    out.time.esp_time_valid =
        sources.time_sync_service.esp_clock_valid();
    out.time.esp_time_source =
        sources.time_sync_service.esp_clock_source_name();
    sources.time_sync_service.utc_now_iso(out.time.esp_datetime,
                                          sizeof(out.time.esp_datetime));

    return out;
}

}  // namespace aircannect
