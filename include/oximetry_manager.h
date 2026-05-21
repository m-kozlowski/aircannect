#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include <stdint.h>

#include "app_config.h"
#include "board.h"
#include "oximetry_types.h"

namespace aircannect {

enum class OximetrySource {
    None,
    Udp,
};

struct OximetryReading {
    int16_t spo2 = -1;
    int16_t pulse_bpm = -1;
    bool valid = false;
    uint32_t timestamp_ms = 0;
};

struct OximetryStatus {
    bool enabled = false;
    OximetryAdvertiseMode advertise_mode = OximetryAdvertiseMode::Auto;
    bool source_present = false;
    bool source_fresh = false;
    OximetrySource source = OximetrySource::None;
    char source_detail[48] = {};
    OximetryReading reading;
    uint16_t udp_port = AC_OXIMETRY_UDP_PORT;
    bool udp_started = false;
    bool ble_available = false;
    bool advertising = false;
    bool connected = false;
    bool subscribed = false;
    bool manual_advertising_requested = false;
    bool pairing_active = false;
    uint32_t pairing_left_ms = 0;
    uint32_t last_source_age_ms = 0;
    uint32_t udp_packets = 0;
    uint32_t udp_bad_packets = 0;
    uint32_t ble_connections = 0;
    uint32_t ble_disconnects = 0;
    uint32_t ble_last_disconnect_reason = 0;
    uint32_t ble_notifications = 0;
    uint32_t ble_invalid_notifications = 0;
    char ble_name[AC_OXIMETRY_BLE_NAME_MAX + 1] = {};
    char ble_peer[24] = {};
    char last_error[64] = {};
};

class OximetryManager {
public:
    bool begin(AppConfig &app_config);
    void poll(bool network_available);

    bool set_enabled(bool enabled);
    bool set_advertise_mode(OximetryAdvertiseMode mode);
    bool request_advertising(bool enabled);
    bool request_pairing(bool enabled);
    bool forget_bonds();

    OximetryStatus status() const;
    void print_status(Print &out) const;

private:
    void apply_config();
    void build_ble_name();
    void set_error(const char *text);

    bool ensure_udp(bool network_available);
    void stop_udp();
    void poll_udp(uint32_t now_ms);
    void note_udp_packet(uint16_t spo2_raw,
                         uint16_t pulse_raw,
                         IPAddress remote_ip,
                         uint32_t now_ms);

    bool ensure_ble();
    void rebuild_advertising_data();
    bool as11_advertising_requested(uint32_t now_ms) const;
    bool ble_runtime_required(uint32_t now_ms) const;
    void update_advertising_policy(uint32_t now_ms);
    void update_pairing_state(uint32_t now_ms);
    void start_advertising();
    void stop_advertising();
    void disconnect_ble();
    void stop_ble_roles();
    void stop_ble_roles_if_idle(uint32_t now_ms);
    void notify_ble(uint32_t now_ms);
    void drain_ble_events();
    void enforce_source_required(uint32_t now_ms);

    void mark_source_stale(uint32_t now_ms);
    bool source_alive(uint32_t now_ms) const;
    bool sample_fresh(uint32_t now_ms) const;

    static int16_t decode_plx_sfloat(uint16_t raw, bool &valid);
    static uint16_t encode_plx_sfloat_int(int16_t value);

    friend class PlxBleServerCallbacks;
    friend class PlxBleMeasurementCallbacks;
    void on_ble_connect(uint16_t conn_handle,
                        const char *peer,
                        bool bonded);
    void on_ble_disconnect(uint16_t conn_handle, int reason);
    void on_ble_subscribe(uint16_t conn_handle, bool enabled);
    void on_ble_error(const char *text);

    AppConfig *app_config_ = nullptr;
    OximetryStatus status_;
    bool begun_ = false;
    bool ble_initialized_ = false;
    bool ble_adv_data_dirty_ = true;
    bool source_present_ = false;
    OximetrySource source_ = OximetrySource::None;
    OximetryReading reading_;
    uint32_t last_source_ms_ = 0;
    uint32_t last_notify_ms_ = 0;
    uint32_t last_config_check_ms_ = 0;
    uint32_t pairing_until_ms_ = 0;
    uint32_t no_source_connected_since_ms_ = 0;
    char ble_name_[AC_OXIMETRY_BLE_NAME_MAX + 1] = {};
    char last_hostname_[64] = {};
    WiFiUDP udp_;

#if AC_OXIMETRY_BLE_ENABLED
    portMUX_TYPE ble_event_mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
    bool ble_connect_pending_ = false;
    uint16_t ble_pending_conn_handle_ = UINT16_MAX;
    bool ble_pending_bonded_ = false;
    char ble_pending_peer_[24] = {};
    bool ble_disconnect_pending_ = false;
    uint16_t ble_pending_disconnect_handle_ = UINT16_MAX;
    int ble_pending_disconnect_reason_ = 0;
    bool ble_subscribe_pending_ = false;
    uint16_t ble_pending_subscribe_handle_ = UINT16_MAX;
    bool ble_pending_subscribe_enabled_ = false;
    bool ble_error_pending_ = false;
    char ble_pending_error_[64] = {};
};

}  // namespace aircannect
