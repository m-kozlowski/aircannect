#include "oximetry_internal.h"

#include <string.h>

#include "debug_log.h"

namespace aircannect {

class PlxBleServerCallbacks : public NimBLEServerCallbacks {
public:
    explicit PlxBleServerCallbacks(OximetryManager *owner)
        : owner_(owner) {}

    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn_info) override {
        const bool have_bonds = NimBLEDevice::getNumBonds() > 0;
        const bool known =
            conn_info.isBonded() ||
            NimBLEDevice::isBonded(conn_info.getIdAddress()) ||
            NimBLEDevice::isBonded(conn_info.getAddress());
        std::string peer = conn_info.getIdAddress().toString();
        if (peer == "00:00:00:00:00:00") {
            peer = conn_info.getAddress().toString();
        }

        if (have_bonds && !known) {
            if (owner_) owner_->on_ble_error("unknown BLE central rejected");
            server->disconnect(conn_info);
            return;
        }

        if (owner_) {
            owner_->on_ble_connect(conn_info.getConnHandle(), peer.c_str(),
                                   known);
        }
    }

    void onDisconnect(NimBLEServer *server,
                      NimBLEConnInfo &conn_info,
                      int reason) override {
        (void)server;
        if (owner_) owner_->on_ble_disconnect(conn_info.getConnHandle(),
                                              reason);
    }

    void onAuthenticationComplete(NimBLEConnInfo &conn_info) override {
        if (!owner_) return;
        std::string peer = conn_info.getIdAddress().toString();
        if (peer == "00:00:00:00:00:00") {
            peer = conn_info.getAddress().toString();
        }
        owner_->on_ble_connect(conn_info.getConnHandle(), peer.c_str(),
                               conn_info.isBonded());
    }

private:
    OximetryManager *owner_ = nullptr;
};

class PlxBleMeasurementCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit PlxBleMeasurementCallbacks(OximetryManager *owner)
        : owner_(owner) {}

    void onSubscribe(NimBLECharacteristic *characteristic,
                     NimBLEConnInfo &conn_info,
                     uint16_t sub_value) override {
        (void)characteristic;
        if (owner_) {
            owner_->on_ble_subscribe(conn_info.getConnHandle(),
                                     (sub_value & 0x0001) != 0);
        }
    }

private:
    OximetryManager *owner_ = nullptr;
};

bool OximetryManager::forget_bonds() {
#if AC_OXIMETRY_BLE_ENABLED
    disconnect_ble();
    const bool ok = NimBLEDevice::deleteAllBonds();
    if (!ok) set_error("bond delete failed");
    return ok;
#else
    set_error("BLE disabled");
    return false;
#endif
}

bool OximetryManager::ensure_ble() {
    if (!status_.enabled) return false;
#if AC_OXIMETRY_BLE_ENABLED
    if (ble_initialized_) {
        if (ble_adv_data_dirty_) rebuild_advertising_data();
        return true;
    }

    if (!ensure_ble_runtime(ble_name_)) {
        set_error("BLE init failed");
        return false;
    }

    ble_server = NimBLEDevice::getServer();
    if (!ble_server) ble_server = NimBLEDevice::createServer();
    if (!ble_server) {
        set_error("BLE server alloc failed");
        return false;
    }
    ble_server->setCallbacks(new PlxBleServerCallbacks(this), true);

    NimBLEService *service = ble_server->createService("1822");
    if (!service) {
        set_error("PLX service alloc failed");
        return false;
    }
    plx_features =
        service->createCharacteristic("2A60", NIMBLE_PROPERTY::READ, 2);
    plx_continuous =
        service->createCharacteristic("2A5F", NIMBLE_PROPERTY::NOTIFY, 5);
    if (!plx_features || !plx_continuous) {
        set_error("PLX characteristic alloc failed");
        return false;
    }
    const uint8_t features[] = {0x00, 0x00};
    plx_features->setValue(features, sizeof(features));
    plx_continuous->setCallbacks(new PlxBleMeasurementCallbacks(this));
    if (!ble_server->start()) {
        set_error("BLE server start failed");
        return false;
    }

    ble_initialized_ = true;
    status_.ble_available = true;
    rebuild_advertising_data();
    Log::logf(CAT_OXI, LOG_INFO,
              "[OXI] PLX BLE peripheral ready name=%s\n", ble_name_);
    return true;
#else
    status_.ble_available = false;
    set_error("BLE disabled");
    return false;
#endif
}

void OximetryManager::rebuild_advertising_data() {
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
    const size_t name_len = strnlen(ble_name_, AC_OXIMETRY_BLE_NAME_MAX);
    raw[len++] = static_cast<uint8_t>(name_len + 1);
    raw[len++] = 0x09;
    memcpy(raw + len, ble_name_, name_len);
    len += name_len;

    NimBLEAdvertisementData adv_data;
    adv_data.addData(raw, len);
    advertising->enableScanResponse(false);
    advertising->setAdvertisementData(adv_data);
    ble_adv_data_dirty_ = false;

    if (was_advertising) advertising->start();
#endif
}

bool OximetryManager::as11_advertising_requested(uint32_t now_ms) const {
    if (!status_.enabled || !status_.ble_available) return false;
    return
        status_.pairing_active ||
        (sample_fresh(now_ms) &&
         (status_.advertise_mode == OximetryAdvertiseMode::Auto ||
          status_.manual_advertising_requested));
}

bool OximetryManager::ble_runtime_required(uint32_t now_ms) const {
    if (!status_.enabled || !status_.ble_available) return false;
    if (status_.connected || status_.advertising) return true;

    // Future BLE sensor-central support belongs here. A known sensor scan or
    // sensor connection should keep NimBLE up even when the AS11-facing PLX
    // peripheral is idle.
    return as11_advertising_requested(now_ms);
}

void OximetryManager::update_advertising_policy(uint32_t now_ms) {
    if (!status_.enabled || !status_.ble_available) {
        stop_advertising();
        stop_ble_roles_if_idle(now_ms);
        return;
    }
    if (status_.connected) return;

    if (as11_advertising_requested(now_ms)) start_advertising();
    else {
        stop_advertising();
        stop_ble_roles_if_idle(now_ms);
    }
}

void OximetryManager::update_pairing_state(uint32_t now_ms) {
    if (!status_.pairing_active) return;
    if (static_cast<int32_t>(pairing_until_ms_ - now_ms) > 0) return;

    status_.pairing_active = false;
    pairing_until_ms_ = 0;
    if (!source_present_) {
        disconnect_ble();
        stop_advertising();
        stop_ble_roles_if_idle(now_ms);
        Log::logf(CAT_OXI, LOG_INFO, "[OXI] pairing window expired\n");
    }
}

void OximetryManager::start_advertising() {
#if AC_OXIMETRY_BLE_ENABLED
    if (status_.advertising) return;
    if (!ensure_ble()) return;
    if (ble_adv_data_dirty_) rebuild_advertising_data();
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (!advertising || !advertising->start()) {
        set_error("BLE advertising failed");
        return;
    }
    status_.advertising = true;
    Log::logf(CAT_OXI, LOG_INFO, "[OXI] BLE advertising started\n");
#endif
}

void OximetryManager::stop_advertising() {
#if AC_OXIMETRY_BLE_ENABLED
    if (!status_.advertising) return;
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (advertising) advertising->stop();
#endif
    status_.advertising = false;
}

void OximetryManager::disconnect_ble() {
#if AC_OXIMETRY_BLE_ENABLED
    if (ble_server) {
        std::vector<uint16_t> peers = ble_server->getPeerDevices();
        for (uint16_t handle : peers) ble_server->disconnect(handle);
    }
#endif
    status_.connected = false;
    status_.subscribed = false;
    ble_conn_handle = BLE_CONN_NONE;
    no_source_connected_since_ms_ = 0;
}

void OximetryManager::stop_ble_roles() {
    stop_advertising();
    disconnect_ble();
    status_.advertising = false;
    status_.connected = false;
    status_.subscribed = false;
    ble_conn_handle = BLE_CONN_NONE;
    no_source_connected_since_ms_ = 0;
}

void OximetryManager::stop_ble_roles_if_idle(uint32_t now_ms) {
    if (ble_runtime_required(now_ms)) return;
    stop_ble_roles();
}

void OximetryManager::notify_ble(uint32_t now_ms) {
#if AC_OXIMETRY_BLE_ENABLED
    if (!status_.connected || !status_.subscribed || !plx_continuous) {
        return;
    }
    if (static_cast<int32_t>(now_ms - last_notify_ms_) <
        static_cast<int32_t>(AC_OXIMETRY_NOTIFY_INTERVAL_MS)) {
        return;
    }
    last_notify_ms_ = now_ms;

    uint16_t spo2 = PLX_SFLOAT_NAN;
    uint16_t pulse = PLX_SFLOAT_NAN;
    if (sample_fresh(now_ms) && reading_.valid) {
        spo2 = encode_plx_sfloat_int(reading_.spo2);
        pulse = encode_plx_sfloat_int(reading_.pulse_bpm);
    } else {
        status_.ble_invalid_notifications++;
    }

    uint8_t payload[5] = {
        0x00,
        static_cast<uint8_t>(spo2 & 0xff),
        static_cast<uint8_t>((spo2 >> 8) & 0xff),
        static_cast<uint8_t>(pulse & 0xff),
        static_cast<uint8_t>((pulse >> 8) & 0xff),
    };
    if (plx_continuous->notify(payload, sizeof(payload), ble_conn_handle)) {
        status_.ble_notifications++;
    }
#endif
}

void OximetryManager::drain_ble_events() {
    bool connect_pending = false;
    uint16_t connect_handle = BLE_CONN_NONE;
    bool connect_bonded = false;
    char connect_peer[sizeof(status_.ble_peer)] = {};

    bool disconnect_pending = false;
    uint16_t disconnect_handle = BLE_CONN_NONE;
    int disconnect_reason = 0;

    bool subscribe_pending = false;
    uint16_t subscribe_handle = BLE_CONN_NONE;
    bool subscribe_enabled = false;

    bool error_pending = false;
    char error_text[sizeof(status_.last_error)] = {};

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&ble_event_mux_);
#endif
    connect_pending = ble_connect_pending_;
    connect_handle = ble_pending_conn_handle_;
    connect_bonded = ble_pending_bonded_;
    strncpy(connect_peer, ble_pending_peer_, sizeof(connect_peer) - 1);
    connect_peer[sizeof(connect_peer) - 1] = 0;

    disconnect_pending = ble_disconnect_pending_;
    disconnect_handle = ble_pending_disconnect_handle_;
    disconnect_reason = ble_pending_disconnect_reason_;

    subscribe_pending = ble_subscribe_pending_;
    subscribe_handle = ble_pending_subscribe_handle_;
    subscribe_enabled = ble_pending_subscribe_enabled_;

    error_pending = ble_error_pending_;
    strncpy(error_text, ble_pending_error_, sizeof(error_text) - 1);
    error_text[sizeof(error_text) - 1] = 0;

    ble_connect_pending_ = false;
    ble_disconnect_pending_ = false;
    ble_subscribe_pending_ = false;
    ble_error_pending_ = false;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&ble_event_mux_);
#endif

    if (error_pending) set_error(error_text);

    if (connect_pending) {
        (void)connect_bonded;
        if (!status_.connected || ble_conn_handle != connect_handle) {
            status_.ble_connections++;
        }
        status_.connected = true;
        if (status_.pairing_active) {
            status_.pairing_active = false;
            pairing_until_ms_ = 0;
        }
        status_.advertising = false;
        ble_conn_handle = connect_handle;
        strncpy(status_.ble_peer, connect_peer,
                sizeof(status_.ble_peer) - 1);
        status_.ble_peer[sizeof(status_.ble_peer) - 1] = 0;
    }

    if (subscribe_pending) {
        ble_conn_handle = subscribe_handle;
        status_.subscribed = subscribe_enabled;
    }

    if (disconnect_pending) {
        (void)disconnect_handle;
        status_.connected = false;
        status_.subscribed = false;
        status_.ble_disconnects++;
        status_.ble_last_disconnect_reason =
            static_cast<uint32_t>(disconnect_reason);
        ble_conn_handle = BLE_CONN_NONE;
        no_source_connected_since_ms_ = 0;
    }
}

void OximetryManager::enforce_source_required(uint32_t now_ms) {
    if (!status_.connected || status_.pairing_active || source_present_) {
        no_source_connected_since_ms_ = 0;
        return;
    }

    if (!no_source_connected_since_ms_) {
        no_source_connected_since_ms_ = now_ms;
        return;
    }

    if (static_cast<int32_t>(now_ms - no_source_connected_since_ms_) <
        static_cast<int32_t>(AC_OXIMETRY_SOURCE_TIMEOUT_MS)) {
        return;
    }

    disconnect_ble();
    stop_advertising();
    stop_ble_roles_if_idle(now_ms);
    Log::logf(CAT_OXI, LOG_INFO,
              "[OXI] no source after BLE connect; disconnected\n");
}

void OximetryManager::on_ble_connect(uint16_t conn_handle,
                                     const char *peer,
                                     bool bonded) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&ble_event_mux_);
#endif
    ble_connect_pending_ = true;
    ble_pending_conn_handle_ = conn_handle;
    ble_pending_bonded_ = bonded;
    if (peer) {
        strncpy(ble_pending_peer_, peer, sizeof(ble_pending_peer_) - 1);
        ble_pending_peer_[sizeof(ble_pending_peer_) - 1] = 0;
    } else {
        ble_pending_peer_[0] = 0;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&ble_event_mux_);
#endif
}

void OximetryManager::on_ble_disconnect(uint16_t conn_handle, int reason) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&ble_event_mux_);
#endif
    ble_disconnect_pending_ = true;
    ble_pending_disconnect_handle_ = conn_handle;
    ble_pending_disconnect_reason_ = reason;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&ble_event_mux_);
#endif
}

void OximetryManager::on_ble_subscribe(uint16_t conn_handle, bool enabled) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&ble_event_mux_);
#endif
    ble_subscribe_pending_ = true;
    ble_pending_subscribe_handle_ = conn_handle;
    ble_pending_subscribe_enabled_ = enabled;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&ble_event_mux_);
#endif
}

void OximetryManager::on_ble_error(const char *text) {
    if (!text) text = "";
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&ble_event_mux_);
#endif
    ble_error_pending_ = true;
    strncpy(ble_pending_error_, text, sizeof(ble_pending_error_) - 1);
    ble_pending_error_[sizeof(ble_pending_error_) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&ble_event_mux_);
#endif
}

}  // namespace aircannect
