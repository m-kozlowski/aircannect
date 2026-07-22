#include "system_status_snapshot.h"

#include <esp_system.h>
#include <stdio.h>

#include "version.h"

namespace aircannect {

namespace {

StorageStatus cached_storage_status;
bool cached_storage_status_valid = false;

StorageStatus collect_storage_status_snapshot() {
    StorageStatus storage;
    if (Storage::try_status(storage)) {
        cached_storage_status = storage;
        cached_storage_status_valid = true;
        return storage;
    }
    if (cached_storage_status_valid) return cached_storage_status;
    return Storage::status();
}

}  // namespace

const char *system_reset_reason_name() {
    switch (esp_reset_reason()) {
        case ESP_RST_UNKNOWN: return "unknown";
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        case ESP_RST_USB: return "usb";
        case ESP_RST_JTAG: return "jtag";
        case ESP_RST_EFUSE: return "efuse";
        case ESP_RST_PWR_GLITCH: return "power_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default: return "other";
    }
}

SystemStatusSnapshot collect_system_status(
    const SystemStatusSources &sources,
    SystemStatusCheckpoint checkpoint) {
    SystemStatusSnapshot out;
    out.now_ms = millis();
    out.uptime_s = out.now_ms / 1000;
    out.version = aircannect_version();
    out.built = aircannect_build_date();
    out.reset_reason = system_reset_reason_name();
    if (checkpoint) checkpoint("web_ui.snapshots.status.core");

    out.memory = Memory::status();
    if (checkpoint) checkpoint("web_ui.snapshots.status.memory");
    out.storage = collect_storage_status_snapshot();
    if (checkpoint) checkpoint("web_ui.snapshots.status.storage");

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
    if (checkpoint) checkpoint("web_ui.snapshots.status.wifi");

    out.ota_active = sources.firmware_installer.active();
    out.update = sources.update_checker.notification();
    if (checkpoint) checkpoint("web_ui.snapshots.status.ota");

    const As11DeviceState &as11 = sources.device.state();
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
    if (checkpoint) checkpoint("web_ui.snapshots.status.as11");

    out.oximetry = sources.oximetry_manager.runtime_status();
    if (checkpoint) checkpoint("web_ui.snapshots.status.oxi");

    out.time.resmed_time_sync_enabled =
        sources.app_config.resmed_time_sync_enabled;
    out.time.ntp_synced = sources.time_sync_service.ntp_synced();
    out.time.esp_time_valid =
        sources.time_sync_service.esp_clock_valid();
    out.time.esp_time_source =
        sources.time_sync_service.esp_clock_source_name();
    sources.time_sync_service.utc_now_iso(out.time.esp_datetime,
                                          sizeof(out.time.esp_datetime));
    if (checkpoint) checkpoint("web_ui.snapshots.status.time");

    return out;
}

}  // namespace aircannect
