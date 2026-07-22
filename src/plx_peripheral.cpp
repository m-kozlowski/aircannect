#include "plx_peripheral.h"

#include <string.h>

#include "debug_log.h"
#include "oximetry_codec.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
class PlxBleServerCallbacks : public NimBLEServerCallbacks {
public:
    explicit PlxBleServerCallbacks(PlxPeripheral *owner) : owner_(owner) {}

    void onConnect(NimBLEServer *server, NimBLEConnInfo &connection) override {
        const bool have_bonds = NimBLEDevice::getNumBonds() > 0;
        const bool known =
            connection.isBonded() ||
            NimBLEDevice::isBonded(connection.getIdAddress()) ||
            NimBLEDevice::isBonded(connection.getAddress());
        std::string peer = connection.getIdAddress().toString();
        if (peer == "00:00:00:00:00:00") {
            peer = connection.getAddress().toString();
        }

        if (have_bonds && !known) {
            if (owner_) owner_->callback_error("unknown BLE central rejected");
            server->disconnect(connection);
            return;
        }
        if (owner_) {
            owner_->callback_connected(connection.getConnHandle(),
                                       peer.c_str(), known);
        }
    }

    void onDisconnect(NimBLEServer *server,
                      NimBLEConnInfo &connection,
                      int reason) override {
        (void)server;
        if (owner_) {
            owner_->callback_disconnected(connection.getConnHandle(), reason);
        }
    }

    void onAuthenticationComplete(NimBLEConnInfo &connection) override {
        if (!owner_) return;

        std::string peer = connection.getIdAddress().toString();
        if (peer == "00:00:00:00:00:00") {
            peer = connection.getAddress().toString();
        }
        owner_->callback_connected(connection.getConnHandle(), peer.c_str(),
                                   connection.isBonded());
    }

private:
    PlxPeripheral *owner_ = nullptr;
};

class PlxBleMeasurementCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit PlxBleMeasurementCallbacks(PlxPeripheral *owner)
        : owner_(owner) {}

    void onSubscribe(NimBLECharacteristic *characteristic,
                     NimBLEConnInfo &connection,
                     uint16_t subscription) override {
        (void)characteristic;
        if (owner_) {
            owner_->callback_subscribed(
                connection.getConnHandle(), (subscription & 0x0001) != 0);
        }
    }

private:
    PlxPeripheral *owner_ = nullptr;
};
#endif

bool PlxPeripheral::begin(bool enabled,
                          OximetryAdvertiseMode advertise_mode,
                          const char *name) {
    status_.ble_available = AC_OXIMETRY_BLE_ENABLED != 0;
    configure(enabled, advertise_mode, name);
    return !enabled || ensure_ble();
}

void PlxPeripheral::configure(bool enabled,
                              OximetryAdvertiseMode advertise_mode,
                              const char *name) {
    const bool mode_changed = status_.advertise_mode != advertise_mode;

    status_.enabled = enabled;
    status_.advertise_mode = advertise_mode;
    if (set_name(name)) {
        advertising_data_dirty_ = true;
    }
    if (mode_changed && advertise_mode == OximetryAdvertiseMode::Auto) {
        status_.manual_advertising_requested = false;
    }

    if (!enabled) {
        status_.pairing_active = false;
        pairing_until_ms_ = 0;
        stop_roles();
    }
}

void PlxPeripheral::poll(const OximetryHubSnapshot &source,
                         uint32_t now_ms) {
    if (!status_.enabled) {
        stop_roles();
        return;
    }
    if (!ensure_ble()) return;

    drain_events();
    update_pairing(source, now_ms);
    enforce_source_required(source, now_ms);
    update_advertising(source);
    notify(source, now_ms);
}

bool PlxPeripheral::request_advertising(bool enabled) {
    status_.manual_advertising_requested = enabled;
    if (!enabled) stop_advertising();
    return true;
}

bool PlxPeripheral::request_pairing(bool enabled) {
    if (enabled) {
        pairing_until_ms_ = millis() + AC_OXIMETRY_PAIRING_WINDOW_MS;
        status_.pairing_active = true;
        status_.manual_advertising_requested = false;
        set_error("");
        return true;
    }

    pairing_until_ms_ = 0;
    status_.pairing_active = false;
    return true;
}

bool PlxPeripheral::forget_bonds() {
#if AC_OXIMETRY_BLE_ENABLED
    if (!ensure_ble()) return false;

    disconnect_central();
    const bool removed = NimBLEDevice::deleteAllBonds();
    if (!removed) set_error("bond delete failed");
    return removed;
#else
    set_error("BLE disabled");
    return false;
#endif
}

PlxPeripheralStatus PlxPeripheral::status(uint32_t now_ms) const {
    PlxPeripheralStatus out = status_;
    if (out.pairing_active &&
        static_cast<int32_t>(pairing_until_ms_ - now_ms) > 0) {
        out.pairing_left_ms = pairing_until_ms_ - now_ms;
    }
    return out;
}

bool PlxPeripheral::ensure_ble() {
    if (!status_.enabled) return false;
#if AC_OXIMETRY_BLE_ENABLED
    if (initialized_) {
        if (advertising_data_dirty_) rebuild_advertising_data();
        return true;
    }
    if (!runtime_.ensure_started(status_.name)) {
        set_error("BLE init failed");
        return false;
    }

    server_ = NimBLEDevice::getServer();
    if (!server_) server_ = NimBLEDevice::createServer();
    if (!server_) {
        set_error("BLE server alloc failed");
        return false;
    }
    server_->setCallbacks(new PlxBleServerCallbacks(this), true);

    NimBLEService *service = server_->createService("1822");
    if (!service) {
        set_error("PLX service alloc failed");
        return false;
    }
    features_ = service->createCharacteristic(
        "2A60", NIMBLE_PROPERTY::READ, 2);
    continuous_ = service->createCharacteristic(
        "2A5F", NIMBLE_PROPERTY::NOTIFY, 5);
    if (!features_ || !continuous_) {
        set_error("PLX characteristic alloc failed");
        return false;
    }

    const uint8_t features[] = {0x00, 0x00};
    features_->setValue(features, sizeof(features));
    continuous_->setCallbacks(new PlxBleMeasurementCallbacks(this));
    if (!server_->start()) {
        set_error("BLE server start failed");
        return false;
    }

    initialized_ = true;
    status_.ble_available = true;
    rebuild_advertising_data();
    Log::logf(CAT_OXI, LOG_INFO,
              "PLX BLE peripheral ready name=%s\n", status_.name);
    return true;
#else
    status_.ble_available = false;
    set_error("BLE disabled");
    return false;
#endif
}

void PlxPeripheral::rebuild_advertising_data() {
#if AC_OXIMETRY_BLE_ENABLED
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (!advertising) return;

    const bool was_advertising = status_.advertising;
    if (was_advertising) advertising->stop();

    uint8_t raw[31] = {};
    size_t len = 0;
    raw[len++] = 0x02;
    raw[len++] = 0x01;
    raw[len++] = 0x02;
    raw[len++] = 0x03;
    raw[len++] = 0x03;
    raw[len++] = 0x22;
    raw[len++] = 0x18;

    const size_t name_len =
        strnlen(status_.name, AC_OXIMETRY_BLE_NAME_MAX);
    raw[len++] = static_cast<uint8_t>(name_len + 1);
    raw[len++] = 0x09;
    memcpy(raw + len, status_.name, name_len);
    len += name_len;

    NimBLEAdvertisementData data;
    data.addData(raw, len);
    advertising->enableScanResponse(false);
    advertising->setAdvertisementData(data);
    advertising_data_dirty_ = false;

    if (was_advertising) advertising->start();
#endif
}

bool PlxPeripheral::advertising_requested(
    const OximetryHubSnapshot &source) const {
    if (!status_.enabled || !status_.ble_available) return false;

    return status_.pairing_active ||
           (source.source_fresh &&
            (status_.advertise_mode == OximetryAdvertiseMode::Auto ||
             status_.manual_advertising_requested));
}

void PlxPeripheral::update_advertising(
    const OximetryHubSnapshot &source) {
    if (!status_.enabled || !status_.ble_available) {
        stop_advertising();
        return;
    }
    if (status_.connected) return;

    if (advertising_requested(source)) start_advertising();
    else stop_advertising();
}

void PlxPeripheral::update_pairing(const OximetryHubSnapshot &source,
                                   uint32_t now_ms) {
    if (!status_.pairing_active ||
        static_cast<int32_t>(pairing_until_ms_ - now_ms) > 0) {
        return;
    }

    status_.pairing_active = false;
    pairing_until_ms_ = 0;
    if (!source.source_present) {
        disconnect_central();
        stop_advertising();
        Log::logf(CAT_OXI, LOG_INFO, "pairing window expired\n");
    }
}

void PlxPeripheral::start_advertising() {
#if AC_OXIMETRY_BLE_ENABLED
    if (status_.advertising || !ensure_ble()) return;
    if (advertising_data_dirty_) rebuild_advertising_data();

    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (!advertising || !advertising->start()) {
        set_error("BLE advertising failed");
        return;
    }

    status_.advertising = true;
    Log::logf(CAT_OXI, LOG_INFO, "BLE advertising started\n");
#endif
}

void PlxPeripheral::stop_advertising() {
#if AC_OXIMETRY_BLE_ENABLED
    if (!status_.advertising) return;

    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (advertising) advertising->stop();
#endif
    status_.advertising = false;
}

void PlxPeripheral::disconnect_central() {
#if AC_OXIMETRY_BLE_ENABLED
    if (server_) {
        const std::vector<uint16_t> peers = server_->getPeerDevices();
        for (uint16_t handle : peers) server_->disconnect(handle);
    }
    connection_handle_ = UINT16_MAX;
#endif
    status_.connected = false;
    status_.subscribed = false;
    no_source_since_ms_ = 0;
}

void PlxPeripheral::stop_roles() {
    stop_advertising();
    disconnect_central();
}

void PlxPeripheral::notify(const OximetryHubSnapshot &source,
                           uint32_t now_ms) {
#if AC_OXIMETRY_BLE_ENABLED
    if (!status_.connected || !status_.subscribed || !continuous_) return;
    if (static_cast<int32_t>(now_ms - last_notify_ms_) <
        static_cast<int32_t>(AC_OXIMETRY_NOTIFY_INTERVAL_MS)) {
        return;
    }
    last_notify_ms_ = now_ms;

    uint16_t spo2 = PLX_SFLOAT_NAN;
    uint16_t pulse = PLX_SFLOAT_NAN;
    if (source.source_fresh && source.reading.valid) {
        spo2 = encode_sfloat_int_value(source.reading.spo2);
        pulse = encode_sfloat_int_value(source.reading.pulse_bpm);
    } else {
        status_.invalid_notifications++;
    }

    const uint8_t payload[5] = {
        0x00,
        static_cast<uint8_t>(spo2 & 0xff),
        static_cast<uint8_t>((spo2 >> 8) & 0xff),
        static_cast<uint8_t>(pulse & 0xff),
        static_cast<uint8_t>((pulse >> 8) & 0xff),
    };
    if (continuous_->notify(payload, sizeof(payload), connection_handle_)) {
        status_.notifications++;
    }
#else
    (void)source;
    (void)now_ms;
#endif
}

void PlxPeripheral::enforce_source_required(
    const OximetryHubSnapshot &source,
    uint32_t now_ms) {
    if (!status_.connected || status_.pairing_active ||
        source.source_present) {
        no_source_since_ms_ = 0;
        return;
    }

    if (!no_source_since_ms_) {
        no_source_since_ms_ = now_ms;
        return;
    }
    if (static_cast<int32_t>(now_ms - no_source_since_ms_) <
        static_cast<int32_t>(AC_OXIMETRY_SOURCE_TIMEOUT_MS)) {
        return;
    }

    disconnect_central();
    stop_advertising();
    Log::logf(CAT_OXI, LOG_INFO,
              "no source after BLE connect; disconnected\n");
}

void PlxPeripheral::drain_events() {
    bool connect_pending = false;
    uint16_t connection_handle = UINT16_MAX;
    bool bonded = false;
    char peer[sizeof(status_.peer)] = {};
    bool disconnect_pending = false;
    uint16_t disconnect_handle = UINT16_MAX;
    int disconnect_reason = 0;
    bool subscribe_pending = false;
    uint16_t subscribe_handle = UINT16_MAX;
    bool subscribe_enabled = false;
    bool error_pending = false;
    char error[sizeof(status_.last_error)] = {};

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&event_mux_);
#endif
    connect_pending = connect_pending_;
    connection_handle = pending_connection_handle_;
    bonded = pending_bonded_;
    strncpy(peer, pending_peer_, sizeof(peer) - 1);
    peer[sizeof(peer) - 1] = 0;
    disconnect_pending = disconnect_pending_;
    disconnect_handle = pending_disconnect_handle_;
    disconnect_reason = pending_disconnect_reason_;
    subscribe_pending = subscribe_pending_;
    subscribe_handle = pending_subscribe_handle_;
    subscribe_enabled = pending_subscribe_enabled_;
    error_pending = error_pending_;
    strncpy(error, pending_error_, sizeof(error) - 1);
    error[sizeof(error) - 1] = 0;
    connect_pending_ = false;
    disconnect_pending_ = false;
    subscribe_pending_ = false;
    error_pending_ = false;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&event_mux_);
#endif

    if (error_pending) set_error(error);
    if (connect_pending) {
        (void)bonded;
#if AC_OXIMETRY_BLE_ENABLED
        if (!status_.connected || connection_handle_ != connection_handle) {
            status_.connections++;
        }
        connection_handle_ = connection_handle;
#endif
        status_.connected = true;
        status_.advertising = false;
        strncpy(status_.peer, peer, sizeof(status_.peer) - 1);
        status_.peer[sizeof(status_.peer) - 1] = 0;
        if (status_.pairing_active) {
            status_.pairing_active = false;
            pairing_until_ms_ = 0;
        }
    }
    if (subscribe_pending) {
#if AC_OXIMETRY_BLE_ENABLED
        connection_handle_ = subscribe_handle;
#else
        (void)subscribe_handle;
#endif
        status_.subscribed = subscribe_enabled;
    }
    if (disconnect_pending) {
        (void)disconnect_handle;
        status_.connected = false;
        status_.subscribed = false;
        status_.disconnects++;
        status_.last_disconnect_reason =
            static_cast<uint32_t>(disconnect_reason);
        no_source_since_ms_ = 0;
#if AC_OXIMETRY_BLE_ENABLED
        connection_handle_ = UINT16_MAX;
#endif
    }
}

void PlxPeripheral::callback_connected(uint16_t connection_handle,
                                       const char *peer,
                                       bool bonded) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&event_mux_);
#endif
    connect_pending_ = true;
    pending_connection_handle_ = connection_handle;
    pending_bonded_ = bonded;
    strncpy(pending_peer_, peer ? peer : "", sizeof(pending_peer_) - 1);
    pending_peer_[sizeof(pending_peer_) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&event_mux_);
#endif
}

void PlxPeripheral::callback_disconnected(uint16_t connection_handle,
                                          int reason) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&event_mux_);
#endif
    disconnect_pending_ = true;
    pending_disconnect_handle_ = connection_handle;
    pending_disconnect_reason_ = reason;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&event_mux_);
#endif
}

void PlxPeripheral::callback_subscribed(uint16_t connection_handle,
                                        bool enabled) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&event_mux_);
#endif
    subscribe_pending_ = true;
    pending_subscribe_handle_ = connection_handle;
    pending_subscribe_enabled_ = enabled;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&event_mux_);
#endif
}

void PlxPeripheral::callback_error(const char *text) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&event_mux_);
#endif
    error_pending_ = true;
    strncpy(pending_error_, text ? text : "", sizeof(pending_error_) - 1);
    pending_error_[sizeof(pending_error_) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&event_mux_);
#endif
}

bool PlxPeripheral::set_name(const char *name) {
    char sanitized[sizeof(status_.name)] = {};
    const char *source = name && name[0] ? name : "aircannect";

    size_t write = 0;
    for (size_t read = 0; source[read] &&
                          write < AC_OXIMETRY_BLE_NAME_MAX; ++read) {
        const char character = source[read];
        if (character >= 0x20 && character <= 0x7e) {
            sanitized[write++] = character;
        }
    }
    if (!write) strncpy(sanitized, "aircannect", sizeof(sanitized) - 1);
    if (strcmp(status_.name, sanitized) == 0) return false;

    strncpy(status_.name, sanitized, sizeof(status_.name) - 1);
    status_.name[sizeof(status_.name) - 1] = 0;
    return true;
}

void PlxPeripheral::set_error(const char *text) {
    strncpy(status_.last_error, text ? text : "",
            sizeof(status_.last_error) - 1);
    status_.last_error[sizeof(status_.last_error) - 1] = 0;
}

}  // namespace aircannect
