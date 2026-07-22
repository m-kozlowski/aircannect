#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_oximetry.h"
#include "oximetry_hub.h"
#include "oximetry_types.h"

namespace aircannect {

enum class OximetrySensorState : uint8_t {
    Off,
    Idle,
    Scanning,
    Connecting,
    Connected,
    Streaming,
};

struct OximetrySensorDevice {
    char addr[18] = {};
    uint8_t addr_type = 1;
    char name[AC_OXIMETRY_SENSOR_NAME_MAX + 1] = {};
    int rssi = 0;
    bool autoconnect = true;
};

struct UdpOximeterStatus {
    uint16_t port = 0;
    bool enabled = false;
    bool started = false;
    uint32_t packets = 0;
    uint32_t bad_packets = 0;
    char last_error[48] = {};
};

struct BleSensorStatus {
    OximetrySensorState state = OximetrySensorState::Off;
    bool task_started = false;
#if AC_STACK_PROFILE_ENABLED
    uint32_t task_stack_high_water_bytes = 0;
#endif
    bool scanning = false;
    bool connected = false;
    uint8_t known_count = 0;
    uint8_t scan_count = 0;
    uint32_t scan_generation = 0;
    uint32_t notifications = 0;
    uint32_t invalid_notifications = 0;
    uint32_t connects = 0;
    uint32_t disconnects = 0;
    uint32_t connect_failures = 0;
    uint32_t scans = 0;
    char peer[18] = {};
    char name[AC_OXIMETRY_SENSOR_NAME_MAX + 1] = {};
    char last_error[64] = {};
};

struct PlxPeripheralStatus {
    bool enabled = false;
    OximetryAdvertiseMode advertise_mode = OximetryAdvertiseMode::Auto;
    bool ble_available = false;
    bool advertising = false;
    bool connected = false;
    bool subscribed = false;
    bool manual_advertising_requested = false;
    bool pairing_active = false;
    uint32_t pairing_left_ms = 0;
    uint32_t connections = 0;
    uint32_t disconnects = 0;
    uint32_t last_disconnect_reason = 0;
    uint32_t notifications = 0;
    uint32_t invalid_notifications = 0;
    char name[AC_OXIMETRY_BLE_NAME_MAX + 1] = {};
    char peer[24] = {};
    char last_error[64] = {};
};

struct OximetryRuntimeStatus {
    bool enabled = false;
    OximetryAdvertiseMode advertise_mode = OximetryAdvertiseMode::Auto;
    bool source_present = false;
    bool source_fresh = false;
    OximetrySource source = OximetrySource::None;
    char source_detail[48] = {};
    OximetryReading reading;
    uint16_t udp_port = 0;
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

OximetryRuntimeStatus compose_oximetry_status(
    const OximetryHubSnapshot &hub,
    const UdpOximeterStatus &udp,
    const PlxPeripheralStatus &peripheral);

}  // namespace aircannect
