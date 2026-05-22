#include "oximetry_internal.h"

#include <string.h>

#include "debug_log.h"
#include "string_util.h"

namespace aircannect {

namespace {

const char *source_name(OximetrySource source) {
    switch (source) {
        case OximetrySource::Udp: return "udp";
        case OximetrySource::Ble: return "ble";
        case OximetrySource::None:
        default:
            return "none";
    }
}

}  // namespace

bool OximetryManager::begin(AppConfig &app_config) {
    app_config_ = &app_config;
#if AC_OXIMETRY_BLE_ENABLED
    sensor_owner = this;
    if (!ble_runtime_mutex) ble_runtime_mutex = xSemaphoreCreateMutex();
#endif
    status_.udp_port = app_config.data().oximetry_udp_port;
    status_.advertise_mode = app_config.data().oximetry_advertise_mode;
    status_.enabled = app_config.data().oximetry_enabled;
    build_ble_name();
    status_.ble_available = AC_OXIMETRY_BLE_ENABLED != 0;
    load_sensor_known();
    begun_ = true;
    apply_config();
    return true;
}

void OximetryManager::poll(bool network_available) {
    if (!begun_) return;
    const uint32_t now_ms = millis();
    if (static_cast<int32_t>(now_ms - last_config_check_ms_) >= 500) {
        last_config_check_ms_ = now_ms;
        apply_config();
    }

    if (!status_.enabled) {
        status_.pairing_active = false;
        pairing_until_ms_ = 0;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        sensor_auto_allowed_ = false;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        stop_udp();
        stop_ble_roles();
        return;
    }

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_auto_allowed_ =
        !source_present_ || source_ == OximetrySource::Ble;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    if (has_sensor_autoconnect()) ensure_sensor_task();

    ensure_udp(network_available);
    poll_udp(now_ms);
    drain_sensor_events(now_ms);

    if (source_present_ && !source_alive(now_ms)) {
        mark_source_stale(now_ms);
    }

    if (ble_initialized_) drain_ble_events();
    update_pairing_state(now_ms);
    enforce_source_required(now_ms);
    update_advertising_policy(now_ms);
    notify_ble(now_ms);
}

bool OximetryManager::set_enabled(bool enabled) {
    if (!app_config_) return false;
    const bool ok = app_config_->set_oximetry_enabled(enabled);
    apply_config();
    return ok;
}

bool OximetryManager::set_advertise_mode(OximetryAdvertiseMode mode) {
    if (!app_config_) return false;
    const bool ok = app_config_->set_oximetry_advertise_mode(mode);
    apply_config();
    return ok;
}

bool OximetryManager::request_advertising(bool enabled) {
    status_.manual_advertising_requested = enabled;
    if (!enabled) {
        stop_advertising();
        stop_ble_roles_if_idle(millis());
    }
    return true;
}

bool OximetryManager::request_pairing(bool enabled) {
    if (enabled) {
        pairing_until_ms_ = millis() + AC_OXIMETRY_PAIRING_WINDOW_MS;
        status_.pairing_active = true;
        status_.manual_advertising_requested = false;
        set_error("");
        return true;
    }

    pairing_until_ms_ = 0;
    status_.pairing_active = false;
    if (!source_present_) {
        disconnect_ble();
        stop_advertising();
        stop_ble_roles_if_idle(millis());
    }
    return true;
}

OximetryStatus OximetryManager::status() const {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    OximetryStatus out = status_;
    out.sensor_task_started = sensor_task_started_;
    out.sensor_known_count = 0;
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        if (sensor_known_[i].addr[0]) out.sensor_known_count++;
    }
    out.sensor_scan_count = sensor_scan_count_;
    out.sensor_scan_generation = sensor_scan_generation_;
    strncpy(out.sensor_peer, sensor_connected_addr_,
            sizeof(out.sensor_peer) - 1);
    out.sensor_peer[sizeof(out.sensor_peer) - 1] = 0;
    strncpy(out.sensor_name, sensor_connected_name_,
            sizeof(out.sensor_name) - 1);
    out.sensor_name[sizeof(out.sensor_name) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    const uint32_t now_ms = millis();
    out.source_present = source_present_;
    out.source_fresh = sample_fresh(now_ms);
    out.source = source_;
    out.reading = reading_;
    out.last_source_age_ms =
        source_present_ ? now_ms - last_source_ms_ : 0;
    if (status_.pairing_active) {
        out.pairing_left_ms =
            static_cast<int32_t>(pairing_until_ms_ - now_ms) > 0
                ? pairing_until_ms_ - now_ms
                : 0;
    }
    strncpy(out.ble_name, ble_name_, sizeof(out.ble_name) - 1);
    out.ble_name[sizeof(out.ble_name) - 1] = 0;
    return out;
}

void OximetryManager::apply_config() {
    if (!app_config_) return;
    const AppConfigData &cfg = app_config_->data();
    const bool was_enabled = status_.enabled;
    const uint16_t old_port = status_.udp_port;
    const OximetryAdvertiseMode old_mode = status_.advertise_mode;

    status_.enabled = cfg.oximetry_enabled;
    status_.udp_port = cfg.oximetry_udp_port;
    status_.advertise_mode = cfg.oximetry_advertise_mode;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_enabled_ = cfg.oximetry_enabled;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif

    if (cfg.hostname != last_hostname_) {
        build_ble_name();
        ble_adv_data_dirty_ = true;
    }

#if AC_OXIMETRY_BLE_ENABLED
    if (status_.enabled) {
        (void)ensure_ble();
    }
#endif

    if (!status_.enabled && was_enabled) {
        stop_udp();
        stop_ble_roles();
    }
    if (status_.udp_port != old_port) stop_udp();
    if (status_.advertise_mode != old_mode &&
        status_.advertise_mode == OximetryAdvertiseMode::Auto) {
        status_.manual_advertising_requested = false;
    }
}

void OximetryManager::build_ble_name() {
    const String fallback = "AirCANnect";
    const String &hostname =
        app_config_ ? app_config_->data().hostname : fallback;
    strncpy(last_hostname_, hostname.c_str(), sizeof(last_hostname_) - 1);
    last_hostname_[sizeof(last_hostname_) - 1] = 0;

    size_t write = 0;
    for (size_t i = 0;
         i < hostname.length() && write < AC_OXIMETRY_BLE_NAME_MAX;
         ++i) {
        const char c = hostname[i];
        if (c >= 0x20 && c <= 0x7e) ble_name_[write++] = c;
    }
    if (write == 0) {
        strncpy(ble_name_, "AirCANnect", sizeof(ble_name_) - 1);
    } else {
        ble_name_[write] = 0;
    }
}

void OximetryManager::set_error(const char *text) {
    if (!text) text = "";
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    strncpy(status_.last_error, text, sizeof(status_.last_error) - 1);
    status_.last_error[sizeof(status_.last_error) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
}
























bool OximetryManager::note_source_packet(OximetrySource source,
                                         const char *detail,
                                         uint16_t spo2_raw,
                                         uint16_t pulse_raw,
                                         bool allow_invalid_claim,
                                         bool contact_known,
                                         bool contact_present,
                                         uint32_t now_ms) {
    bool spo2_valid = false;
    bool pulse_valid = false;
    const int16_t spo2 = decode_plx_sfloat(spo2_raw, spo2_valid);
    const int16_t pulse = decode_plx_sfloat(pulse_raw, pulse_valid);
    const bool valid = spo2_valid && pulse_valid;

    if (source_present_ && source_ != source) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] ignored %s sample while %s source is active\n",
                  source_name(source), source_name(source_));
        return false;
    }
    if (!source_present_ && !valid && !allow_invalid_claim) return false;

    const bool source_was_present = source_present_;
    source_present_ = true;
    source_ = source;
    strncpy(status_.source_detail, detail ? detail : "",
            sizeof(status_.source_detail) - 1);
    status_.source_detail[sizeof(status_.source_detail) - 1] = 0;
    last_source_ms_ = now_ms;
    if (source == OximetrySource::Udp) status_.udp_packets++;
    else if (source == OximetrySource::Ble) status_.sensor_notifications++;
    reading_.timestamp_ms = now_ms;
    reading_.valid = valid;
    reading_.contact_known = contact_known;
    reading_.contact_present = contact_present;
    if (reading_.valid) {
        if (source == OximetrySource::Ble) sensor_invalid_since_ms_ = 0;
        reading_.spo2 = spo2;
        reading_.pulse_bpm = pulse;
    } else {
        reading_.spo2 = -1;
        reading_.pulse_bpm = -1;
        if (source == OximetrySource::Ble) {
            status_.sensor_invalid_notifications++;
            if (!sensor_invalid_since_ms_) sensor_invalid_since_ms_ = now_ms;
            if (static_cast<int32_t>(now_ms - sensor_invalid_since_ms_) >=
                static_cast<int32_t>(
                    AC_OXIMETRY_SENSOR_INVALID_DISCONNECT_MS)) {
#if AC_OXIMETRY_BLE_ENABLED
                portENTER_CRITICAL(&sensor_mux_);
#endif
                sensor_disconnect_requested_ = true;
                sensor_disconnect_hold_until_absent_ = true;
#if AC_OXIMETRY_BLE_ENABLED
                portEXIT_CRITICAL(&sensor_mux_);
#endif
                sensor_invalid_since_ms_ = now_ms;
                Log::logf(CAT_OXI, LOG_INFO,
                          "[OXI] Sensor invalid readings; disconnecting\n");
            }
        }
    }
    if (!source_was_present) {
        Log::logf(CAT_OXI, LOG_INFO,
                  "[OXI] source active type=%s detail=%s valid=%s\n",
                  source_name(source_),
                  status_.source_detail[0] ? status_.source_detail : "--",
                  valid ? "yes" : "no");
    }
    return true;
}
















void OximetryManager::mark_source_stale(uint32_t now_ms) {
    (void)now_ms;
    const OximetrySource stale_source = source_;
    source_present_ = false;
    source_ = OximetrySource::None;
    reading_.valid = false;
    reading_.spo2 = -1;
    reading_.pulse_bpm = -1;
    reading_.contact_known = false;
    reading_.contact_present = false;
    sensor_invalid_since_ms_ = 0;
    status_.source_detail[0] = 0;
    if (stale_source == OximetrySource::Ble) {
        sensor_disconnect_requested_ = true;
    }
    if (!status_.pairing_active) {
        disconnect_ble();
        stop_advertising();
        stop_ble_roles_if_idle(now_ms);
        Log::logf(CAT_OXI, LOG_INFO, "[OXI] source stale; BLE stopped\n");
    }
}

bool OximetryManager::source_alive(uint32_t now_ms) const {
    if (!source_present_) return false;
    const uint32_t timeout =
        source_ == OximetrySource::Ble
            ? AC_OXIMETRY_SENSOR_NOTIFY_TIMEOUT_MS
            : AC_OXIMETRY_SOURCE_TIMEOUT_MS;
    return static_cast<int32_t>(now_ms - last_source_ms_) <
           static_cast<int32_t>(timeout);
}

bool OximetryManager::sample_fresh(uint32_t now_ms) const {
    if (!source_present_) return false;
    return static_cast<int32_t>(now_ms - last_source_ms_) <
           static_cast<int32_t>(AC_OXIMETRY_SAMPLE_STALE_MS);
}

int16_t OximetryManager::decode_plx_sfloat(uint16_t raw, bool &valid) {
    return decode_sfloat_int_value(raw, valid);
}

uint16_t OximetryManager::encode_plx_sfloat_int(int16_t value) {
    return encode_sfloat_int_value(value);
}

const char *OximetryManager::sensor_state_name(OximetrySensorState state) {
    switch (state) {
        case OximetrySensorState::Off: return "off";
        case OximetrySensorState::Idle: return "idle";
        case OximetrySensorState::Scanning: return "scanning";
        case OximetrySensorState::Connecting: return "connecting";
        case OximetrySensorState::Connected: return "connected";
        case OximetrySensorState::Streaming: return "streaming";
        default: return "?";
    }
}







}  // namespace aircannect
