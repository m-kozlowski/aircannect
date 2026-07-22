#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "board.h"
#include "oximetry_ble_runtime.h"
#include "oximetry_hub.h"
#include "oximetry_status.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

class PlxBleMeasurementCallbacks;
class PlxBleServerCallbacks;

class PlxPeripheral {
public:
    explicit PlxPeripheral(OximetryBleRuntime &runtime)
        : runtime_(runtime) {}

    bool begin(bool enabled, OximetryAdvertiseMode advertise_mode,
               const char *name);
    void configure(bool enabled, OximetryAdvertiseMode advertise_mode,
                   const char *name);
    void poll(const OximetryHubSnapshot &source, uint32_t now_ms);

    bool request_advertising(bool enabled);
    bool request_pairing(bool enabled);
    bool forget_bonds();

    PlxPeripheralStatus status(uint32_t now_ms) const;

private:
    // BLE service lifecycle
    bool ensure_ble();
    void rebuild_advertising_data();
    bool advertising_requested(const OximetryHubSnapshot &source) const;
    void update_advertising(const OximetryHubSnapshot &source);
    void update_pairing(const OximetryHubSnapshot &source, uint32_t now_ms);
    void start_advertising();
    void stop_advertising();
    void disconnect_central();
    void stop_roles();

    // Data delivery and source policy
    void notify(const OximetryHubSnapshot &source, uint32_t now_ms);
    void enforce_source_required(const OximetryHubSnapshot &source,
                                 uint32_t now_ms);

    // NimBLE callback mailbox
    friend class PlxBleServerCallbacks;
    friend class PlxBleMeasurementCallbacks;
    void callback_connected(uint16_t connection_handle, const char *peer,
                            bool bonded);
    void callback_disconnected(uint16_t connection_handle, int reason);
    void callback_subscribed(uint16_t connection_handle, bool enabled);
    void callback_error(const char *text);
    void drain_events();

    bool set_name(const char *name);
    void set_error(const char *text);

    OximetryBleRuntime &runtime_;
    PlxPeripheralStatus status_;
    bool initialized_ = false;
    bool advertising_data_dirty_ = true;
    uint32_t pairing_until_ms_ = 0;
    uint32_t last_notify_ms_ = 0;
    uint32_t no_source_since_ms_ = 0;

#if AC_OXIMETRY_BLE_ENABLED
    NimBLEServer *server_ = nullptr;
    NimBLECharacteristic *continuous_ = nullptr;
    NimBLECharacteristic *features_ = nullptr;
    uint16_t connection_handle_ = UINT16_MAX;
    portMUX_TYPE event_mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif

    bool connect_pending_ = false;
    uint16_t pending_connection_handle_ = UINT16_MAX;
    bool pending_bonded_ = false;
    char pending_peer_[24] = {};
    bool disconnect_pending_ = false;
    uint16_t pending_disconnect_handle_ = UINT16_MAX;
    int pending_disconnect_reason_ = 0;
    bool subscribe_pending_ = false;
    uint16_t pending_subscribe_handle_ = UINT16_MAX;
    bool pending_subscribe_enabled_ = false;
    bool error_pending_ = false;
    char pending_error_[64] = {};
};

}  // namespace aircannect
