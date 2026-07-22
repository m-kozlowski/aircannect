#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "ble_sensor_protocols.h"
#include "board.h"
#include "oximetry_ble_runtime.h"
#include "oximetry_hub.h"
#include "oximetry_status.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

class SensorBleClientCallbacks;
class SensorBleScanCallbacks;

enum class BleSensorEventKind : uint8_t {
    None,
    Sample,
    Disconnected,
};

struct BleSensorEvent {
    BleSensorEventKind kind = BleSensorEventKind::None;
    OximetrySample sample;
    int disconnect_reason = 0;
    char disconnect_addr[18] = {};
};

class BleSensorSource {
public:
    explicit BleSensorSource(OximetryBleRuntime &runtime)
        : runtime_(runtime) {}

    bool begin(bool enabled, const char *runtime_name);
    void configure(bool enabled, const char *runtime_name);
    void set_auto_allowed(bool allowed);
    bool has_autoconnect() const;

    bool request_scan();
    bool request_connect(const char *addr_or_index);
    bool request_connect(const OximetrySensorDevice &device);
    bool request_disconnect(bool hold_until_absent = true);
    bool forget(const char *addr_or_all);
    bool set_autoconnect(const char *addr, bool enabled);

    bool take_event(BleSensorEvent &event);
    BleSensorStatus status() const;
    size_t scan_results(OximetrySensorDevice *out, size_t max) const;
    size_t known_sensors(OximetrySensorDevice *out, size_t max) const;
#if AC_STACK_PROFILE_ENABLED
    uint32_t task_stack_high_water_bytes() const;
#endif

private:
    // Persistence and commands
    bool load_known();
    bool save_known() const;
    bool find_addr(const char *addr, size_t &index) const;
    bool resolve_target(const char *addr_or_index,
                        OximetrySensorDevice &target) const;

    // Worker lifecycle
    void ensure_task();
    static void task_entry(void *param);
    void task_loop();
    void set_state(OximetrySensorState state);
    void set_error(const char *text);

    // Scan and connection policy
    void hold_autoconnect(const char *addr, uint32_t now_ms,
                          bool until_absent);
    bool autoconnect_holdoff_active(uint32_t now_ms) const;
    void store_scan_result(const char *addr, uint8_t addr_type,
                           const char *name, int rssi);
    bool pick_autoconnect_target(OximetrySensorDevice &target, uint32_t now_ms);
    bool connect_target(const OximetrySensorDevice &target, bool manual);
    bool subscribe_client(void *client, const char *name);

    // NimBLE callback targets
    friend class SensorBleScanCallbacks;
    friend class SensorBleClientCallbacks;
    static void protocol_sample_callback(void *context, uint16_t spo2_raw,
                                         uint16_t pulse_raw,
                                         bool invalid,
                                         bool contact_known,
                                         bool contact_present);
    void publish_sample(uint16_t spo2_raw, uint16_t pulse_raw,
                        bool from_invalid_packet,
                        bool contact_known,
                        bool contact_present);
    void callback_disconnected(int reason);

    OximetryBleRuntime &runtime_;
    BleSensorProtocolEngine protocols_;
#if AC_OXIMETRY_BLE_ENABLED
    NimBLEClient *client_ = nullptr;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif

    BleSensorStatus status_;
    OximetrySensorDevice known_[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    OximetrySensorDevice scan_results_[AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
    uint8_t scan_count_ = 0;
    uint32_t scan_generation_ = 0;
    bool known_loaded_ = false;

#if AC_STACK_PROFILE_ENABLED
    TaskHandle_t task_ = nullptr;
#endif
    bool task_started_ = false;
    bool scan_requested_ = false;
    bool manual_connect_requested_ = false;
    bool disconnect_requested_ = false;
    bool disconnect_hold_until_absent_ = false;
    char manual_target_[18] = {};
    OximetrySensorDevice manual_target_device_;
    char connected_addr_[18] = {};
    char connected_name_[AC_OXIMETRY_SENSOR_NAME_MAX + 1] = {};
    char auto_holdoff_addr_[18] = {};
    uint32_t auto_holdoff_until_ms_ = 0;
    bool auto_holdoff_until_absent_ = false;
    bool auto_allowed_ = false;
    bool enabled_ = false;
    char runtime_name_[AC_OXIMETRY_BLE_NAME_MAX + 1] = {};

    bool sample_pending_ = false;
    OximetrySample pending_sample_;
    bool disconnect_pending_ = false;
    int pending_disconnect_reason_ = 0;
    char pending_disconnect_addr_[18] = {};
};

}  // namespace aircannect
