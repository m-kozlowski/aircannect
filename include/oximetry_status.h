#pragma once

#include <stddef.h>
#include <stdint.h>
#include <cstring>
#include <string_view>

#include "board_oximetry.h"
#include "board_storage.h"
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
    bool recordings = false;

    bool oxyii_ring() const {
        const std::string_view n(name);
        for (const auto model : {std::string_view("O2Ring S"), std::string_view("O2Ring-S"),
                                 std::string_view("SHQO2Pro"), std::string_view("S8-AW")}) {
            if (n == model || (n.size() > model.size() && n.substr(0, model.size()) == model &&
                               (n[model.size()] == ' ' || n[model.size()] == '_'))) return true;
        }
        return n.substr(0, 6) == "T8520_";
    }

    bool supports_recordings() const {
        const std::string_view n(name);
        const bool checkme_max = n.size() == 8 && n.substr(0, 4) == "O2M " &&
                                 n.find_first_not_of("0123456789", 4) == n.npos;
        return AC_STORAGE_HAS_SDCARD &&
               (oxyii_ring() || checkme_max || std::strcmp(name, "O2Ring") == 0 ||
                (std::strncmp(name, "O2Ring ", 7) == 0 && name[7] >= '0' && name[7] <= '9'));
    }
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
    bool scanning = false;
    bool observing = false;
    bool connected = false;
    uint8_t known_count = 0;
    uint8_t scan_count = 0;
    uint32_t scan_generation = 0;
    uint32_t notifications = 0;
    uint32_t invalid_notifications = 0;
    uint32_t sample_queue_drops = 0;
    uint32_t notification_drops = 0;
    uint32_t connects = 0;
    uint32_t disconnects = 0;
    uint32_t connect_failures = 0;
    uint32_t scans = 0;
    uint32_t observations = 0;
    char peer[18] = {};
    char name[AC_OXIMETRY_SENSOR_NAME_MAX + 1] = {};
    char last_error[64] = {};
};

struct PlxPeripheralStatus {
    bool enabled = false;
    bool integration_available = false;
    OximetryAdvertiseMode advertise_mode = OximetryAdvertiseMode::Auto;
    bool ble_available = false;
    bool advertising = false;
    bool connected = false;
    bool subscribed = false;
    bool manual_advertising_requested = false;
    bool pairing_active = false;
    uint32_t pairing_left_ms = 0;
    uint32_t last_disconnect_reason = 0;
    char name[AC_OXIMETRY_BLE_NAME_MAX + 1] = {};
    char peer[24] = {};
    char last_error[64] = {};
};

struct OximetryRuntimeStatus {
    bool enabled = false;
    bool airsense_integration_available = false;
    OximetryAdvertiseMode advertise_mode = OximetryAdvertiseMode::Auto;
    bool source_present = false;
    bool source_fresh = false;
    OximetrySource source = OximetrySource::None;
    char source_detail[48] = {};
    OximetryReading reading;
    uint32_t csv_lost = 0;
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
    uint32_t ble_last_disconnect_reason = 0;
    char ble_name[AC_OXIMETRY_BLE_NAME_MAX + 1] = {};
    char ble_peer[24] = {};
    char last_error[64] = {};
};

OximetryRuntimeStatus compose_oximetry_status(
    const OximetryHubSnapshot &hub,
    const UdpOximeterStatus &udp,
    const PlxPeripheralStatus &peripheral, uint32_t csv_lost);

}  // namespace aircannect
