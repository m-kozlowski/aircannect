#include "system_status_snapshot.h"

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
    out.storage_writer = StorageWriter::status();

    out.wifi.stats = sources.wifi_manager.stats();
    out.wifi.state = sources.wifi_manager.state_name();
    out.wifi.ssid = sources.wifi_manager.sta_ssid().c_str();
    out.wifi.ip = sources.wifi_manager.ip().toString().c_str();
    char bssid[AC_WIFI_BSSID_TEXT_MAX];
    sources.wifi_manager.bssid(bssid, sizeof(bssid));
    out.wifi.bssid = bssid;
    out.wifi.softap_mode = sources.wifi_manager.softap_mode();
    out.wifi.softap_running = sources.wifi_manager.softap_running();
    out.wifi.roaming_enabled = sources.wifi_manager.roaming_enabled();
    out.wifi.roaming_suspended = sources.wifi_manager.roaming_suspended();
    out.wifi.rssi = sources.wifi_manager.rssi();
    out.wifi.channel = sources.wifi_manager.channel();
    out.wifi.active_profile =
        sources.wifi_manager.active_profile_index();

    out.tcp_started = sources.tcp_bridge.started();
    out.ota_active = sources.ota_manager.active();
    out.ota_ready = sources.ota_manager.status().http_ready;

    const As11DeviceState &as11 = sources.arbiter.as11_state();
    out.as11.product_name = as11.product_name();
    out.as11.serial_number = as11.serial_number();
    out.as11.software_identifier = as11.software_identifier();
    out.as11.active_therapy_profile = as11.active_therapy_profile();
    out.as11.motor_run_meter = as11.mhr();
    out.as11.rop = as11.rop();
    out.as11.last_activity_event = as11.last_activity_event();
    out.as11.last_activity_event_report_time =
        as11.last_activity_event_report_time();
    out.as11.last_activity_event_ms = as11.last_activity_event_ms();
    out.as11.device_datetime = as11.device_datetime();
    out.as11.therapy_state = as11.therapy_state();
    out.as11.pending_therapy_target = as11.pending_therapy_target();
    out.as11.clock_valid = as11.clock_valid();
    out.as11.clock_sample_ms = as11.clock_sample_ms();
    out.as11.clock_offset_valid = as11.clock_offset_valid();
    out.as11.clock_offset_ms = as11.clock_offset_ms();
    out.as11.timezone_offset_valid = as11.timezone_offset_valid();
    out.as11.timezone_offset_minutes =
        as11.timezone_offset_minutes();

    out.session = sources.session_manager.status();
    out.sink = sources.sink_manager.status();
    out.oximetry = sources.oximetry_manager.runtime_status();

    out.time.resmed_time_sync_enabled =
        sources.app_config.data().resmed_time_sync_enabled;
    out.time.status = sources.time_sync_service.last_status();
    out.time.ntp_synced = sources.time_sync_service.ntp_synced();
    out.time.esp_time_valid =
        sources.time_sync_service.esp_clock_valid();
    out.time.esp_time_source =
        sources.time_sync_service.esp_clock_source_name();
    out.time.esp_datetime = sources.time_sync_service.utc_now_iso();

    return out;
}

}  // namespace aircannect
