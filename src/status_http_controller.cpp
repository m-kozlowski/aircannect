#include "status_http_controller.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <stdio.h>
#include <string_view>

#include "as11_device_service.h"
#include "board.h"
#include "config_service.h"
#include "firmware_installer.h"
#include "json_util.h"
#include "oximetry_manager.h"
#include "storage_manager.h"
#include "system_status_snapshot.h"
#include "time_sync_service.h"
#include "update_checker.h"
#include "wifi_manager.h"

namespace aircannect {
namespace {

bool motor_hours(std::string_view iso_duration,
                 char *out,
                 size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    if (iso_duration.size() < 4 || iso_duration.substr(0, 2) != "PT") {
        return false;
    }

    const size_t start = 2;
    const size_t end = iso_duration.find('S', start);
    if (end == std::string_view::npos || end == start) return false;

    uint64_t seconds = 0;
    for (size_t i = start; i < end; ++i) {
        const char c = iso_duration[i];
        if (c < '0' || c > '9') return false;
        seconds = seconds * 10 + static_cast<unsigned>(c - '0');
    }

    const unsigned long hours =
        static_cast<unsigned long>((seconds + 1800) / 3600);
    snprintf(out, out_size, "%lu", hours);
    return true;
}

const char *oximetry_source_name(OximetrySource source) {
    switch (source) {
        case OximetrySource::None: return "none";
        case OximetrySource::Udp: return "udp";
        case OximetrySource::Ble: return "ble";
        default: return "unknown";
    }
}

bool build_status_json(LargeTextBuffer &json,
                       const As11DeviceService &device,
                       const WifiManager &wifi_manager,
                       const ConfigService &config,
                       const TimeSyncService &time_sync,
                       const FirmwareInstaller &installer,
                       const UpdateChecker &update_checker,
                       const OximetryManager &oximetry,
                       StatusHttpController::PollCheckpoint checkpoint) {
    const SystemStatusSnapshot snap = collect_system_status({
        device,
        wifi_manager,
        config.data(),
        time_sync,
        installer,
        update_checker,
        oximetry,
    }, checkpoint);
    const MemoryStatus &mem = snap.memory;
    const StorageStatus &storage = snap.storage;
    const WifiStatusSnapshot &wifi = snap.wifi;
    const As11StatusSnapshot &as11 = snap.as11;
    const OximetryRuntimeStatus &oxi = snap.oximetry;
    const TimeStatusSnapshot &time = snap.time;

    json = "{";
    json_add_string(json, "version", snap.version, false);
    json_add_string(json, "built", snap.built);
    json_add_string(json, "hostname", config.data().hostname.c_str());
    json_add_int(json, "uptime", snap.uptime_s);
    json_add_int(json, "heap", static_cast<long>(mem.heap_free));
    json_add_bool(json, "psram_available", mem.psram_available);
    json_add_int(json, "psram_free", static_cast<long>(mem.psram_free));
    json_add_string(json, "storage_state",
                    Storage::state_name(storage.state));
    json_add_uint64(json, "storage_total", storage.total_bytes);
    json_add_uint64(json, "storage_used", storage.used_bytes);
    json_add_string_view(json, "wifi_state", wifi.state);
    json_add_string_view(json, "wifi_ssid", wifi.ssid);
    json_add_string(json, "wifi_ip", wifi.ip);
    json_add_int(json, "wifi_rssi", wifi.rssi);
    json_add_int(json, "wifi_channel", wifi.channel);
    json_add_string(json, "wifi_bssid", wifi.bssid);
    json_add_int(json, "wifi_profile", wifi.active_profile);
    json_add_bool(json, "wifi_roam", wifi.roaming_enabled);
    json_add_bool(json, "update_checking", snap.update.checking);
    json_add_bool(json, "update_available", snap.update.available);
    json_add_string(json, "update_version", snap.update.version);
    json_add_string_view(json, "device_name", as11.product_name);
    json_add_string_view(json, "serial", as11.serial_number);
    json_add_string_view(json, "software_id", as11.software_identifier);
    json_add_string(json, "therapy",
                    As11DeviceState::therapy_state_name(as11.therapy_state));
    json_add_string(json, "therapy_pending",
                    As11DeviceState::therapy_target_name(
                        as11.pending_therapy_target));
    json_add_string_view(json, "profile", as11.active_therapy_profile);

    char hours[16];
    motor_hours(as11.motor_run_meter, hours, sizeof(hours));
    json_add_string(json, "motor_hours", hours);

    json += ",\"oximetry\":{";
    json_add_bool(json, "enabled", oxi.enabled, false);
    json_add_string(json, "source", oximetry_source_name(oxi.source));
    json_add_string(json, "source_detail", oxi.source_detail);
    json_add_bool(json, "source_present", oxi.source_present);
    json_add_bool(json, "source_fresh", oxi.source_fresh);
    json_add_bool(json, "valid", oxi.reading.valid);
    json_add_bool(json, "contact_known", oxi.reading.contact_known);
    json_add_bool(json, "contact_present", oxi.reading.contact_present);
    if (oxi.reading.valid) {
        json_add_int(json, "spo2", oxi.reading.spo2);
        json_add_int(json, "pulse_bpm", oxi.reading.pulse_bpm);
    } else {
        json += ",\"spo2\":null,\"pulse_bpm\":null";
    }
    json_add_int(json, "source_age_ms", oxi.last_source_age_ms);
    json_add_string(json, "advertise_mode",
                    oximetry_advertise_mode_name(oxi.advertise_mode));
    json_add_bool(json, "manual_advertising_requested",
                  oxi.manual_advertising_requested);
    json_add_bool(json, "ble_available", oxi.ble_available);
    json_add_bool(json, "advertising", oxi.advertising);
    json_add_bool(json, "connected", oxi.connected);
    json_add_bool(json, "subscribed", oxi.subscribed);
    json_add_bool(json, "pairing_active", oxi.pairing_active);
    json_add_int(json, "pairing_left_ms", oxi.pairing_left_ms);
    json_add_string(json, "ble_name", oxi.ble_name);
    json_add_string(json, "ble_peer", oxi.ble_peer);
    json += '}';

    json_add_string_view(json, "device_datetime", as11.device_datetime);
    if (as11.clock_valid) {
        json_add_int(json, "device_datetime_age_ms",
                     snap.now_ms - as11.clock_sample_ms);
    } else {
        json += ",\"device_datetime_age_ms\":null";
    }
    json_add_bool(json, "resmed_time_sync_enabled",
                  time.resmed_time_sync_enabled);
    json_add_bool(json, "ntp_synced", time.ntp_synced);
    json_add_bool(json, "esp_time_valid", time.esp_time_valid);
    json_add_string_view(json, "esp_time_source", time.esp_time_source);
    json_add_string(json, "esp_datetime", time.esp_datetime);
    json += '}';
    return !json.overflowed();
}

}  // namespace

bool StatusHttpController::begin(As11DeviceService &device,
                                 WifiManager &wifi,
                                 ConfigService &config,
                                 TimeSyncService &time_sync,
                                 FirmwareInstaller &installer,
                                 UpdateChecker &update_checker,
                                 OximetryManager &oximetry) {
    device_ = &device;
    wifi_ = &wifi;
    config_ = &config;
    time_sync_ = &time_sync;
    installer_ = &installer;
    update_checker_ = &update_checker;
    oximetry_ = &oximetry;
    observed_device_revision_ = device.revision();
    observed_config_revision_ = config.revision();

    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!cache_mutex_) return false;

    snapshot_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    build_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    return publish_snapshot();
}

void StatusHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/status"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_snapshot(request);
    });
}

void StatusHttpController::poll(PollCheckpoint checkpoint) {
    if (!device_ || !wifi_ || !config_ || !time_sync_ || !installer_ ||
        !update_checker_ || !oximetry_) {
        return;
    }

    if (observed_device_revision_ != device_->revision()) {
        observed_device_revision_ = device_->revision();
        snapshot_dirty_ = true;
    }
    if (observed_config_revision_ != config_->revision()) {
        observed_config_revision_ = config_->revision();
        snapshot_dirty_ = true;
    }

    const uint32_t now_ms = millis();
    const bool periodic_due =
        static_cast<int32_t>(now_ms - last_snapshot_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    if (snapshot_dirty_ || periodic_due) {
        (void)publish_snapshot(checkpoint);
    }
}

bool StatusHttpController::copy_snapshot(LargeTextBuffer &out,
                                         uint32_t &revision) const {
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        return false;
    }

    out.clear();
    const bool copied = out.append(snapshot_json_.c_str(),
                                   snapshot_json_.length());
    if (copied) revision = revision_;
    xSemaphoreGive(cache_mutex_);
    return copied;
}

bool StatusHttpController::publish_snapshot(PollCheckpoint checkpoint) {
    build_json_.clear();
    if (!build_status_json(build_json_, *device_, *wifi_, *config_,
                           *time_sync_, *installer_, *update_checker_,
                           *oximetry_, checkpoint)) {
        return false;
    }

    if (xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return false;
    snapshot_json_.swap(build_json_);
    last_snapshot_ms_ = millis();
    snapshot_dirty_ = false;
    revision_++;
    if (revision_ == 0) revision_ = 1;
    xSemaphoreGive(cache_mutex_);
    return true;
}

void StatusHttpController::send_snapshot(
    AsyncWebServerRequest *request) const {
    if (xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache_busy\"}");
        return;
    }

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (response) {
        response->write(
            reinterpret_cast<const uint8_t *>(snapshot_json_.c_str()),
            snapshot_json_.length());
    }
    xSemaphoreGive(cache_mutex_);

    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    request->send(response);
}

}  // namespace aircannect
