#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include <stdint.h>

#include "app_config.h"
#include "board.h"
#include "oximetry_types.h"

namespace aircannect {

class PlxBleMeasurementCallbacks;
class PlxBleServerCallbacks;
class SensorBleClientCallbacks;
class SensorBleScanCallbacks;

enum class OximetrySource {
    None,
    Udp,
    Ble,
};

enum class OximetrySensorState : uint8_t {
    Off,
    Idle,
    Scanning,
    Connecting,
    Connected,
    Streaming,
};

struct OximetryReading {
    int16_t spo2 = -1;
    int16_t pulse_bpm = -1;
    bool valid = false;
    bool contact_known = false;
    bool contact_present = false;
    uint32_t timestamp_ms = 0;
};

struct OximetrySensorDevice {
    char addr[18] = {};
    uint8_t addr_type = 1;
    char name[AC_OXIMETRY_SENSOR_NAME_MAX + 1] = {};
    int rssi = 0;
    bool autoconnect = true;
};

struct OximetryRuntimeStatus {
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

struct OximetrySensorStatus {
    OximetrySensorState sensor_state = OximetrySensorState::Off;
    bool sensor_task_started = false;
#if AC_STACK_PROFILE_ENABLED
    uint32_t sensor_task_stack_high_water_bytes = 0;
#endif
    bool sensor_scanning = false;
    bool sensor_connected = false;
    uint8_t sensor_known_count = 0;
    uint8_t sensor_scan_count = 0;
    uint32_t sensor_scan_generation = 0;
    uint32_t sensor_notifications = 0;
    uint32_t sensor_invalid_notifications = 0;
    uint32_t sensor_connects = 0;
    uint32_t sensor_disconnects = 0;
    uint32_t sensor_connect_failures = 0;
    uint32_t sensor_scans = 0;
    char sensor_peer[18] = {};
    char sensor_name[AC_OXIMETRY_SENSOR_NAME_MAX + 1] = {};
};

struct OximetryInternalState : public OximetryRuntimeStatus,
                               public OximetrySensorStatus {};

class OximetryManager {
public:
    bool begin(AppConfig &app_config);
    void poll(bool network_available);

    bool set_enabled(bool enabled);
    bool set_advertise_mode(OximetryAdvertiseMode mode);
    bool request_advertising(bool enabled);
    bool request_pairing(bool enabled);
    bool forget_bonds();
    bool request_sensor_scan();
    bool request_sensor_connect(const char *addr_or_index);
    bool request_sensor_connect_device(const OximetrySensorDevice &device);
    bool request_sensor_disconnect();
    bool forget_sensor(const char *addr_or_all);
    bool set_sensor_autoconnect(const char *addr, bool enabled);
#if AC_STACK_PROFILE_ENABLED
    uint32_t sensor_task_stack_high_water_bytes() const;
#endif

    OximetryRuntimeStatus runtime_status() const;
    OximetrySensorStatus sensor_status() const;
    size_t sensor_scan_results(OximetrySensorDevice *out, size_t max) const;
    size_t known_sensors(OximetrySensorDevice *out, size_t max) const;
    void on_sensor_sample(uint16_t spo2_raw,
                          uint16_t pulse_raw,
                          bool from_invalid_packet,
                          bool contact_known = false,
                          bool contact_present = false);
    void on_sensor_disconnect(int reason);

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
    bool note_source_packet(OximetrySource source,
                            const char *detail,
                            uint16_t spo2_raw,
                            uint16_t pulse_raw,
                            bool allow_invalid_claim,
                            bool contact_known,
                            bool contact_present,
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
    void drain_sensor_events(uint32_t now_ms);
    void enforce_source_required(uint32_t now_ms);

    void mark_source_stale(uint32_t now_ms);
    bool source_alive(uint32_t now_ms) const;
    bool sample_fresh(uint32_t now_ms) const;

    static int16_t decode_plx_sfloat(uint16_t raw, bool &valid);
    static uint16_t encode_plx_sfloat_int(int16_t value);
    static const char *sensor_state_name(OximetrySensorState state);

    bool load_sensor_known();
    bool save_sensor_known() const;
    bool has_sensor_autoconnect() const;
    bool find_sensor_addr(const char *addr, size_t &index) const;
    bool resolve_sensor_target(const char *addr_or_index,
                               OximetrySensorDevice &target) const;
    void ensure_sensor_task();
    static void sensor_task_entry(void *param);
    void sensor_task_loop();
    void sensor_set_state(OximetrySensorState state);
    void sensor_hold_autoconnect(const char *addr,
                                 uint32_t now_ms,
                                 bool until_absent);
    bool sensor_autoconnect_holdoff_active(uint32_t now_ms) const;
    void sensor_store_scan_result(const char *addr,
                                  uint8_t addr_type,
                                  const char *name,
                                  int rssi);
    bool sensor_pick_autoconnect_target(OximetrySensorDevice &target,
                                        uint32_t now_ms);
    bool sensor_connect_target(const OximetrySensorDevice &target,
                               bool manual);
    bool sensor_subscribe_client(void *client,
                                 const char *name);
    friend class PlxBleServerCallbacks;
    friend class PlxBleMeasurementCallbacks;
    friend class SensorBleScanCallbacks;
    friend class SensorBleClientCallbacks;
    void on_ble_connect(uint16_t conn_handle,
                        const char *peer,
                        bool bonded);
    void on_ble_disconnect(uint16_t conn_handle, int reason);
    void on_ble_subscribe(uint16_t conn_handle, bool enabled);
    void on_ble_error(const char *text);

    AppConfig *app_config_ = nullptr;
    OximetryInternalState status_;
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
    OximetrySensorDevice sensor_known_[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    OximetrySensorDevice sensor_scan_results_[
        AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
    uint8_t sensor_scan_count_ = 0;
    uint32_t sensor_scan_generation_ = 0;
    bool sensor_known_loaded_ = false;
#if AC_STACK_PROFILE_ENABLED
    TaskHandle_t sensor_task_ = nullptr;
#endif
    bool sensor_task_started_ = false;
    bool sensor_scan_requested_ = false;
    bool sensor_manual_connect_requested_ = false;
    bool sensor_disconnect_requested_ = false;
    bool sensor_disconnect_hold_until_absent_ = false;
    char sensor_manual_target_[18] = {};
    OximetrySensorDevice sensor_manual_target_device_;
    char sensor_connected_addr_[18] = {};
    char sensor_connected_name_[AC_OXIMETRY_SENSOR_NAME_MAX + 1] = {};
    char sensor_auto_holdoff_addr_[18] = {};
    uint32_t sensor_auto_holdoff_until_ms_ = 0;
    bool sensor_auto_holdoff_until_absent_ = false;
    uint32_t sensor_invalid_since_ms_ = 0;
    bool sensor_auto_allowed_ = false;
    bool sensor_enabled_ = false;

#if AC_OXIMETRY_BLE_ENABLED
    portMUX_TYPE ble_event_mux_ = portMUX_INITIALIZER_UNLOCKED;
    portMUX_TYPE sensor_mux_ = portMUX_INITIALIZER_UNLOCKED;
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
    bool sensor_sample_pending_ = false;
    uint16_t sensor_pending_spo2_raw_ = 0x07ff;
    uint16_t sensor_pending_pulse_raw_ = 0x07ff;
    bool sensor_pending_invalid_packet_ = false;
    bool sensor_pending_contact_known_ = false;
    bool sensor_pending_contact_present_ = false;
    bool sensor_disconnect_pending_ = false;
    int sensor_pending_disconnect_reason_ = 0;
    char sensor_pending_disconnect_addr_[18] = {};
};

}  // namespace aircannect
