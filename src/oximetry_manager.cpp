#include "oximetry_manager.h"

#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "string_util.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

namespace {

static constexpr uint16_t PLX_SFLOAT_NAN = 0x07ff;
static constexpr uint16_t PLX_SFLOAT_NRES = 0x0800;
static constexpr uint16_t PLX_SFLOAT_POS_INF = 0x07fe;
static constexpr uint16_t PLX_SFLOAT_NEG_INF = 0x0802;
static constexpr uint16_t PLX_SFLOAT_RESERVED = 0x0801;
static constexpr uint16_t BLE_CONN_NONE = 0xffff;
static constexpr const char *SENSOR_NS = "oxi_sensor";
static constexpr const char *SENSOR_KNOWN_COUNT_KEY = "known_count";
static constexpr const char *PLX_SERVICE_UUID = "1822";
static constexpr const char *PLX_CONTINUOUS_UUID = "2A5F";
static constexpr const char *PLX_SPOT_UUID = "2A5E";
static constexpr const char *NONIN_SERVICE_UUID =
    "46A970E0-0D5F-11E2-8B5E-0002A5D5C51B";
static constexpr const char *NONIN_CONTINUOUS_UUID =
    "0AAD7EA0-0D60-11E2-8E3C-0002A5D5C51B";
static constexpr const char *VIATOM_SERVICE_UUID =
    "14839AC4-7D7E-415C-9A42-167340CF2339";
static constexpr const char *VIATOM_READ_UUID =
    "0734594A-A8E7-4B1A-A6B1-CD5243059A57";
static constexpr const char *VIATOM_WRITE_UUID =
    "8B00ACE7-EB0B-49B0-BBE9-9AEE0A26E1A3";
static constexpr uint8_t VIATOM_CMD_CONFIG = 0x16;
static constexpr uint8_t VIATOM_CMD_INFO = 0x14;
static constexpr uint8_t VIATOM_CMD_READ_SENSORS = 0x17;
static constexpr uint32_t VIATOM_SENSOR_POLL_MS = 2000;
static constexpr uint32_t VIATOM_RESPONSE_TIMEOUT_MS = 1500;
static constexpr size_t VIATOM_WRITE_CHUNK_LEN = 20;
static constexpr uint32_t VIATOM_WRITE_CHUNK_DELAY_MS = 50;
static constexpr uint8_t VIATOM_NO_PENDING_CMD = 0xff;

#if AC_OXIMETRY_BLE_ENABLED
NimBLEServer *ble_server = nullptr;
NimBLECharacteristic *plx_continuous = nullptr;
NimBLECharacteristic *plx_features = nullptr;
uint16_t ble_conn_handle = BLE_CONN_NONE;
NimBLEClient *sensor_client = nullptr;
NimBLERemoteCharacteristic *sensor_viatom_write = nullptr;
uint8_t sensor_viatom_rx[64] = {};
size_t sensor_viatom_rx_len = 0;
size_t sensor_viatom_rx_want = 0;
uint8_t sensor_viatom_pending_cmd = VIATOM_NO_PENDING_CMD;
uint32_t sensor_viatom_pending_ms = 0;
bool sensor_viatom_need_info = false;
bool sensor_viatom_need_time_sync = false;
SemaphoreHandle_t ble_runtime_mutex = nullptr;
OximetryManager *sensor_owner = nullptr;
#endif

uint16_t encode_sfloat_int_value(int16_t value) {
    if (value < 0 || value > 0x07fd) return PLX_SFLOAT_NAN;
    return static_cast<uint16_t>(value) & 0x0fff;
}

uint8_t crc8_ccitt(const uint8_t *data, size_t len, uint8_t crc = 0x00) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

void sensor_known_key(char *out,
                      size_t out_len,
                      size_t index,
                      const char *field) {
    snprintf(out, out_len, "known%u_%s",
             static_cast<unsigned>(index),
             field);
}

void remove_nvs_key_if_present(Preferences &prefs, const char *key) {
    if (prefs.isKey(key)) prefs.remove(key);
}

void viatom_reset_rx() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_viatom_rx_len = 0;
    sensor_viatom_rx_want = 0;
#endif
}

void viatom_clear_pending() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_viatom_pending_cmd = VIATOM_NO_PENDING_CMD;
    sensor_viatom_pending_ms = 0;
#endif
}

const char *viatom_cmd_name(uint8_t cmd) {
    switch (cmd) {
        case VIATOM_CMD_INFO: return "info";
        case VIATOM_CMD_CONFIG: return "config";
        case VIATOM_CMD_READ_SENSORS: return "read_sensors";
        case VIATOM_NO_PENDING_CMD: return "none";
        default: return "unknown";
    }
}

void log_oxi_hex_debug(const char *label,
                       const uint8_t *data,
                       size_t len,
                       uint8_t cmd = VIATOM_NO_PENDING_CMD) {
    if (Log::get_cat_level(CAT_OXI) < LOG_DEBUG) return;
    if (!data) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] %s len=0 pending=%s\n",
                  label ? label : "hex",
                  viatom_cmd_name(cmd));
        return;
    }

    static constexpr size_t MAX_BYTES = 24;
    const size_t shown = len < MAX_BYTES ? len : MAX_BYTES;
    char hex[MAX_BYTES * 3 + 1] = {};
    size_t pos = 0;
    for (size_t i = 0; i < shown && pos + 4 < sizeof(hex); ++i) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
    }
    if (pos > 0) hex[pos - 1] = 0;
    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] %s len=%u pending=%s%s %s\n",
              label ? label : "hex",
              static_cast<unsigned>(len),
              viatom_cmd_name(cmd),
              len > shown ? " truncated" : "",
              hex);
}

bool viatom_decode_reading(const uint8_t *packet,
                           size_t len,
                           uint16_t &spo2_raw,
                           uint16_t &pulse_raw,
                           bool &invalid) {
    if (!packet || len < 9 || packet[0] != 0x55) return false;
    if (packet[1] != static_cast<uint8_t>(packet[2] ^ 0xff)) return false;

    const uint16_t payload_len =
        static_cast<uint16_t>(packet[5]) |
        (static_cast<uint16_t>(packet[6]) << 8);
    const size_t packet_len = static_cast<size_t>(payload_len) + 8;
    if (payload_len < 2 || len < packet_len) return false;
    if (crc8_ccitt(packet, packet_len - 1) != packet[packet_len - 1]) {
        return false;
    }

    const uint8_t spo2 = packet[7];
    const uint8_t pulse = packet[8];
    const bool finger_present = payload_len < 12 || packet[18] != 0;
    const bool valid =
        spo2 > 0 && spo2 <= 100 && pulse > 0 && pulse != 0xff &&
        pulse < 250 && finger_present;
    spo2_raw = valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN;
    pulse_raw = valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN;
    invalid = !valid;
    return true;
}

#if AC_OXIMETRY_BLE_ENABLED
bool viatom_write_packet(NimBLERemoteCharacteristic *write_chr,
                         uint8_t cmd,
                         const uint8_t *payload,
                         size_t payload_len) {
    if (!write_chr || payload_len > 0xffff) return false;
    const size_t packet_len = 7 + payload_len + 1;
    if (packet_len > 80) return false;

    uint8_t packet[80] = {};
    packet[0] = 0xaa;
    packet[1] = cmd;
    packet[2] = static_cast<uint8_t>(cmd ^ 0xff);
    packet[3] = 0x00;
    packet[4] = 0x00;
    packet[5] = static_cast<uint8_t>(payload_len & 0xff);
    packet[6] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    if (payload && payload_len) {
        memcpy(packet + 7, payload, payload_len);
    }
    packet[7 + payload_len] = crc8_ccitt(packet, 7 + payload_len);

    for (size_t offset = 0; offset < packet_len;
         offset += VIATOM_WRITE_CHUNK_LEN) {
        size_t chunk_len = packet_len - offset;
        if (chunk_len > VIATOM_WRITE_CHUNK_LEN) {
            chunk_len = VIATOM_WRITE_CHUNK_LEN;
        }
        if (!write_chr->writeValue(packet + offset, chunk_len, false)) {
            return false;
        }
        if (offset + chunk_len < packet_len) {
            vTaskDelay(pdMS_TO_TICKS(VIATOM_WRITE_CHUNK_DELAY_MS));
        }
    }
    return true;
}

bool viatom_sync_datetime(NimBLERemoteCharacteristic *write_chr) {
    if (!write_chr) return false;

    const time_t now = time(nullptr);
    if (now < 1704067200) return false;  // 2024-01-01T00:00:00Z

    struct tm local = {};
    localtime_r(&now, &local);

    char payload[48] = {};
    if (!strftime(payload, sizeof(payload),
                  "{\"SetTIME\":\"%Y-%m-%d,%H:%M:%S\"}", &local)) {
        return false;
    }

    const bool ok = viatom_write_packet(
        write_chr,
        VIATOM_CMD_CONFIG,
        reinterpret_cast<const uint8_t *>(payload),
        strlen(payload));
    if (ok) {
        Log::logf(CAT_OXI, LOG_INFO,
                  "[OXI] Sensor Viatom datetime set: %s\n",
                  payload);
    } else {
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor Viatom datetime write failed\n");
    }
    return ok;
}

bool viatom_send_command(NimBLERemoteCharacteristic *write_chr,
                         uint8_t cmd,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint32_t now_ms) {
    if (!viatom_write_packet(write_chr, cmd, payload, payload_len)) {
        return false;
    }
    sensor_viatom_pending_cmd = cmd;
    sensor_viatom_pending_ms = now_ms;
    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] Sensor Viatom TX cmd=%s payload_len=%u\n",
              viatom_cmd_name(cmd),
              static_cast<unsigned>(payload_len));
    return true;
}
#endif

#if AC_OXIMETRY_BLE_ENABLED
bool ensure_ble_runtime(const char *name) {
    if (!ble_runtime_mutex) ble_runtime_mutex = xSemaphoreCreateMutex();
    if (ble_runtime_mutex) xSemaphoreTake(ble_runtime_mutex, portMAX_DELAY);

    bool ok = true;
    if (!NimBLEDevice::isInitialized()) {
        ok = NimBLEDevice::init(name ? name : "AirCANnect");
        if (ok) {
            NimBLEDevice::setSecurityAuth(true, false, false);
            NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        }
    }

    if (ble_runtime_mutex) xSemaphoreGive(ble_runtime_mutex);
    return ok;
}
#endif

}  // namespace

#if AC_OXIMETRY_BLE_ENABLED
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

class SensorBleScanCallbacks : public NimBLEScanCallbacks {
public:
    explicit SensorBleScanCallbacks(OximetryManager *owner)
        : owner_(owner) {}

    void onResult(const NimBLEAdvertisedDevice *dev) override {
        if (!owner_ || !dev) return;
        const std::string name = dev->getName();
        const bool is_oxi =
            dev->isAdvertisingService(NimBLEUUID(PLX_SERVICE_UUID)) ||
            dev->isAdvertisingService(NimBLEUUID(NONIN_SERVICE_UUID)) ||
            dev->isAdvertisingService(NimBLEUUID(VIATOM_SERVICE_UUID)) ||
            name.rfind("Nonin", 0) == 0 ||
            name.rfind("O2Ring", 0) == 0 ||
            name.rfind("O2M", 0) == 0 ||
            name.rfind("CheckMe", 0) == 0 ||
            name.rfind("Checkme", 0) == 0 ||
            name.rfind("CheckO2", 0) == 0 ||
            name.rfind("SleepU", 0) == 0 ||
            name.rfind("SleepO2", 0) == 0 ||
            name.rfind("WearO2", 0) == 0 ||
            name.rfind("KidsO2", 0) == 0 ||
            name.rfind("BabyO2", 0) == 0 ||
            name.rfind("Oxylink", 0) == 0;
        if (!is_oxi) return;

        owner_->sensor_store_scan_result(
            dev->getAddress().toString().c_str(),
            dev->getAddress().getType(),
            name.c_str(),
            dev->getRSSI());
    }

private:
    OximetryManager *owner_ = nullptr;
};

class SensorBleClientCallbacks : public NimBLEClientCallbacks {
public:
    explicit SensorBleClientCallbacks(OximetryManager *owner)
        : owner_(owner) {}

    void onDisconnect(NimBLEClient *client, int reason) override {
        (void)client;
        sensor_viatom_write = nullptr;
        viatom_reset_rx();
        viatom_clear_pending();
        sensor_viatom_need_info = false;
        sensor_viatom_need_time_sync = false;
        if (owner_) {
            owner_->sensor_invalid_since_ms_ = 0;
            owner_->on_sensor_disconnect(reason);
        }
    }

    void onPassKeyEntry(NimBLEConnInfo &connInfo) override {
        NimBLEDevice::injectPassKey(connInfo, 0);
    }

    void onConfirmPasskey(NimBLEConnInfo &connInfo, uint32_t pin) override {
        (void)pin;
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

private:
    OximetryManager *owner_ = nullptr;
};

void sensor_plx_notify_cb(NimBLERemoteCharacteristic *chr,
                          uint8_t *data,
                          size_t len,
                          bool is_notify) {
    (void)chr;
    (void)is_notify;
    if (len < 5 || !data) return;
    if (sensor_owner) {
        sensor_owner->on_sensor_sample(
            static_cast<uint16_t>(data[1]) |
                (static_cast<uint16_t>(data[2]) << 8),
            static_cast<uint16_t>(data[3]) |
                (static_cast<uint16_t>(data[4]) << 8),
            false);
    }
}

void sensor_nonin_notify_cb(NimBLERemoteCharacteristic *chr,
                            uint8_t *data,
                            size_t len,
                            bool is_notify) {
    (void)chr;
    (void)is_notify;
    if (len < 5 || !data) return;
    const uint8_t spo2 = data[2];
    const uint16_t pulse =
        static_cast<uint16_t>(data[3]) |
        (static_cast<uint16_t>(data[4]) << 8);
    const bool valid = spo2 > 0 && spo2 <= 100 && pulse > 0 && pulse < 500;
    if (sensor_owner) {
        sensor_owner->on_sensor_sample(
            valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN,
            valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN,
            !valid);
    }
}

void sensor_viatom_notify_cb(NimBLERemoteCharacteristic *chr,
                             uint8_t *data,
                             size_t len,
                             bool is_notify) {
    (void)chr;
    (void)is_notify;
    if (!data || !len) return;
    log_oxi_hex_debug("Sensor Viatom RX fragment",
                      data,
                      len,
                      sensor_viatom_pending_cmd);

    const uint8_t *packet = data;
    size_t packet_len = len;

    if (data[0] == 0x55) {
        sensor_viatom_rx_len = 0;
        sensor_viatom_rx_want = 0;
        if (len >= 7) {
            const uint16_t payload_len =
                static_cast<uint16_t>(data[5]) |
                (static_cast<uint16_t>(data[6]) << 8);
            const size_t want = static_cast<size_t>(payload_len) + 8;
            if (want <= sizeof(sensor_viatom_rx)) {
                sensor_viatom_rx_want = want;
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "[OXI] Sensor Viatom RX too large want=%u\n",
                          static_cast<unsigned>(want));
            }
        }
    }

    bool buffered_packet = false;
    bool buffered_packet_complete = false;
    if (sensor_viatom_rx_want) {
        if (sensor_viatom_rx_len + len > sizeof(sensor_viatom_rx)) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor Viatom RX overflow have=%u add=%u\n",
                      static_cast<unsigned>(sensor_viatom_rx_len),
                      static_cast<unsigned>(len));
            viatom_reset_rx();
            return;
        }
        memcpy(sensor_viatom_rx + sensor_viatom_rx_len, data, len);
        sensor_viatom_rx_len += len;
        if (sensor_viatom_rx_len < sensor_viatom_rx_want) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor Viatom RX partial have=%u want=%u\n",
                      static_cast<unsigned>(sensor_viatom_rx_len),
                      static_cast<unsigned>(sensor_viatom_rx_want));
            return;
        }
        packet = sensor_viatom_rx;
        packet_len = sensor_viatom_rx_len;
        buffered_packet = true;
        buffered_packet_complete =
            sensor_viatom_rx_len >= sensor_viatom_rx_want;
    }

    const uint8_t pending_cmd = sensor_viatom_pending_cmd;
    viatom_clear_pending();
    if (pending_cmd != VIATOM_CMD_READ_SENSORS) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor Viatom RX ignored for cmd=%s\n",
                  viatom_cmd_name(pending_cmd));
        if (buffered_packet_complete) viatom_reset_rx();
        return;
    }

    uint16_t spo2_raw = PLX_SFLOAT_NAN;
    uint16_t pulse_raw = PLX_SFLOAT_NAN;
    bool invalid = true;
    if (!viatom_decode_reading(packet, packet_len, spo2_raw, pulse_raw,
                               invalid)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor Viatom RX read decode failed len=%u\n",
                  static_cast<unsigned>(packet_len));
        if (buffered_packet_complete) viatom_reset_rx();
        return;
    }
    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] Sensor Viatom reading %s\n",
              invalid ? "invalid" : "valid");

    if (buffered_packet) {
        viatom_reset_rx();
    }

    if (sensor_owner) {
        sensor_owner->on_sensor_sample(spo2_raw, pulse_raw, invalid);
    }
}
#endif

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

size_t OximetryManager::sensor_scan_results(OximetrySensorDevice *out,
                                            size_t max) const {
    if (!out || !max) return 0;
    size_t count = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    count = sensor_scan_count_;
    if (count > max) count = max;
    for (size_t i = 0; i < count; ++i) out[i] = sensor_scan_results_[i];
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    return count;
}

size_t OximetryManager::known_sensors(OximetrySensorDevice *out,
                                      size_t max) const {
    if (!out || !max) return 0;
    size_t count = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    for (size_t i = 0;
         i < AC_OXIMETRY_SENSOR_MAX_KNOWN && count < max;
         ++i) {
        if (!sensor_known_[i].addr[0]) continue;
        out[count++] = sensor_known_[i];
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    return count;
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

bool OximetryManager::load_sensor_known() {
    if (sensor_known_loaded_) return true;
    for (auto &dev : sensor_known_) dev = OximetrySensorDevice{};

    Preferences prefs;
    if (!prefs.begin(SENSOR_NS, true)) {
        sensor_known_loaded_ = true;
        return false;
    }

    uint8_t count = prefs.getUChar(SENSOR_KNOWN_COUNT_KEY, 0);
    if (!count && prefs.isKey("count")) {
        count = prefs.getUChar("count", 0);
    }
    for (uint8_t i = 0;
         i < count && i < AC_OXIMETRY_SENSOR_MAX_KNOWN;
        ++i) {
        char key[16];
        sensor_known_key(key, sizeof(key), i, "addr");
        bool has_key = prefs.isKey(key);
        String addr = prefs.getString(key, "");
        if (!has_key) {
            snprintf(key, sizeof(key), "a%u", i);
            addr = prefs.getString(key, "");
        }
        addr.trim();
        if (!addr.length() || addr.length() >= sizeof(sensor_known_[i].addr)) {
            continue;
        }
        strncpy(sensor_known_[i].addr, addr.c_str(),
                sizeof(sensor_known_[i].addr) - 1);

        sensor_known_key(key, sizeof(key), i, "type");
        has_key = prefs.isKey(key);
        sensor_known_[i].addr_type = prefs.getUChar(key, 1);
        if (!has_key) {
            snprintf(key, sizeof(key), "t%u", i);
            sensor_known_[i].addr_type = prefs.getUChar(key, 1);
        }
        sensor_known_key(key, sizeof(key), i, "auto");
        has_key = prefs.isKey(key);
        sensor_known_[i].autoconnect = prefs.getBool(key, true);
        if (!has_key) {
            snprintf(key, sizeof(key), "e%u", i);
            sensor_known_[i].autoconnect = prefs.getBool(key, true);
        }
        sensor_known_key(key, sizeof(key), i, "name");
        has_key = prefs.isKey(key);
        String name = prefs.getString(key, "");
        if (!has_key) {
            snprintf(key, sizeof(key), "n%u", i);
            name = prefs.getString(key, "");
        }
        strncpy(sensor_known_[i].name, name.c_str(),
                sizeof(sensor_known_[i].name) - 1);
    }
    prefs.end();
    sensor_known_loaded_ = true;
    return true;
}

bool OximetryManager::save_sensor_known() const {
    OximetrySensorDevice snapshot[AC_OXIMETRY_SENSOR_MAX_KNOWN];
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        snapshot[i] = sensor_known_[i];
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif

    Preferences prefs;
    if (!prefs.begin(SENSOR_NS, false)) return false;

    uint8_t count = 0;
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        if (snapshot[i].addr[0]) count++;
    }
    bool ok = prefs.putUChar(SENSOR_KNOWN_COUNT_KEY, count) != 0;

    uint8_t out_index = 0;
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        if (!snapshot[i].addr[0]) continue;
        char key[16];
        sensor_known_key(key, sizeof(key), out_index, "addr");
        ok = prefs.putString(key, snapshot[i].addr) != 0 && ok;
        sensor_known_key(key, sizeof(key), out_index, "type");
        ok = prefs.putUChar(key, snapshot[i].addr_type) != 0 && ok;
        sensor_known_key(key, sizeof(key), out_index, "auto");
        ok = prefs.putBool(key, snapshot[i].autoconnect) != 0 && ok;
        sensor_known_key(key, sizeof(key), out_index, "name");
        ok = prefs.putString(key, snapshot[i].name) != 0 && ok;
        out_index++;
    }
    for (; out_index < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++out_index) {
        char key[16];
        sensor_known_key(key, sizeof(key), out_index, "addr");
        remove_nvs_key_if_present(prefs, key);
        sensor_known_key(key, sizeof(key), out_index, "type");
        remove_nvs_key_if_present(prefs, key);
        sensor_known_key(key, sizeof(key), out_index, "auto");
        remove_nvs_key_if_present(prefs, key);
        sensor_known_key(key, sizeof(key), out_index, "name");
        remove_nvs_key_if_present(prefs, key);
    }
    remove_nvs_key_if_present(prefs, "count");
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "a%u", static_cast<unsigned>(i));
        remove_nvs_key_if_present(prefs, key);
        snprintf(key, sizeof(key), "t%u", static_cast<unsigned>(i));
        remove_nvs_key_if_present(prefs, key);
        snprintf(key, sizeof(key), "e%u", static_cast<unsigned>(i));
        remove_nvs_key_if_present(prefs, key);
        snprintf(key, sizeof(key), "n%u", static_cast<unsigned>(i));
        remove_nvs_key_if_present(prefs, key);
    }
    prefs.end();
    return ok;
}

bool OximetryManager::has_sensor_autoconnect() const {
    bool found = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    for (const auto &dev : sensor_known_) {
        if (dev.addr[0] && dev.autoconnect) {
            found = true;
            break;
        }
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    return found;
}

bool OximetryManager::find_sensor_addr(const char *addr, size_t &index) const {
    if (!addr || !addr[0]) return false;
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        if (sensor_known_[i].addr[0] &&
            strcasecmp(sensor_known_[i].addr, addr) == 0) {
            index = i;
            return true;
        }
    }
    return false;
}

bool OximetryManager::resolve_sensor_target(
    const char *addr_or_index,
    OximetrySensorDevice &target) const {
    if (!addr_or_index || !addr_or_index[0]) return false;

    char *end = nullptr;
    const unsigned long parsed = strtoul(addr_or_index, &end, 10);
    if (end && *end == 0) {
        if (parsed >= sensor_scan_count_) return false;
        target = sensor_scan_results_[parsed];
        return target.addr[0] != 0;
    }

    for (uint8_t i = 0; i < sensor_scan_count_; ++i) {
        if (strcasecmp(sensor_scan_results_[i].addr, addr_or_index) == 0) {
            target = sensor_scan_results_[i];
            return true;
        }
    }
    size_t known_index = 0;
    if (find_sensor_addr(addr_or_index, known_index)) {
        target = sensor_known_[known_index];
        return true;
    }
    return false;
}

bool OximetryManager::request_sensor_scan() {
    if (!status_.enabled) return false;
    ensure_sensor_task();
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_scan_requested_ = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    return true;
}

bool OximetryManager::request_sensor_connect(const char *addr_or_index) {
    if (!status_.enabled || !addr_or_index) return false;
    OximetrySensorDevice target;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    const bool ok = resolve_sensor_target(addr_or_index, target);
    if (ok) {
        strncpy(sensor_manual_target_, target.addr,
                sizeof(sensor_manual_target_) - 1);
        sensor_manual_target_[sizeof(sensor_manual_target_) - 1] = 0;
        sensor_manual_target_device_ = target;
        sensor_manual_connect_requested_ = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    if (ok) ensure_sensor_task();
    return ok;
}

bool OximetryManager::request_sensor_disconnect() {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_disconnect_requested_ = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    return true;
}

bool OximetryManager::forget_sensor(const char *addr_or_all) {
    if (!addr_or_all || !addr_or_all[0]) return false;
    bool changed = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    if (strcasecmp(addr_or_all, "all") == 0) {
        for (auto &dev : sensor_known_) dev = OximetrySensorDevice{};
        changed = true;
    } else {
        size_t index = 0;
        if (find_sensor_addr(addr_or_all, index)) {
            sensor_known_[index] = OximetrySensorDevice{};
            changed = true;
        }
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    if (changed) save_sensor_known();
    return changed;
}

bool OximetryManager::set_sensor_autoconnect(const char *addr,
                                             bool enabled) {
    if (!addr || !addr[0]) return false;
    bool changed = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    size_t index = 0;
    if (find_sensor_addr(addr, index)) {
        sensor_known_[index].autoconnect = enabled;
        changed = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    if (changed) save_sensor_known();
    return changed;
}

void OximetryManager::ensure_sensor_task() {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
    if (sensor_task_started_) {
        portEXIT_CRITICAL(&sensor_mux_);
        return;
    }
    sensor_task_started_ = true;
    status_.sensor_task_started = true;
    portEXIT_CRITICAL(&sensor_mux_);
    xTaskCreatePinnedToCore(sensor_task_entry,
                            "oxi_sensor",
                            AC_OXIMETRY_SENSOR_TASK_STACK,
                            this,
                            AC_OXIMETRY_SENSOR_TASK_PRIO,
                            nullptr,
                            0);
#endif
}

void OximetryManager::sensor_task_entry(void *param) {
    auto *self = static_cast<OximetryManager *>(param);
    if (self) self->sensor_task_loop();
    vTaskDelete(nullptr);
}

void OximetryManager::sensor_set_state(OximetrySensorState state) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    status_.sensor_state = state;
    status_.sensor_scanning = state == OximetrySensorState::Scanning;
    status_.sensor_connected =
        state == OximetrySensorState::Connected ||
        state == OximetrySensorState::Streaming;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
}

void OximetryManager::sensor_hold_autoconnect(const char *addr,
                                              uint32_t now_ms) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_auto_holdoff_until_ms_ =
        now_ms + AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS;
    strncpy(sensor_auto_holdoff_addr_, addr ? addr : "",
            sizeof(sensor_auto_holdoff_addr_) - 1);
    sensor_auto_holdoff_addr_[sizeof(sensor_auto_holdoff_addr_) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] Sensor auto-connect holdoff addr=%s ms=%lu\n",
              sensor_auto_holdoff_addr_[0] ? sensor_auto_holdoff_addr_ : "*",
              static_cast<unsigned long>(
                  AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS));
}

void OximetryManager::sensor_store_scan_result(const char *addr,
                                               uint8_t addr_type,
                                               const char *name,
                                               int rssi) {
    if (!addr || !addr[0]) return;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    for (uint8_t i = 0; i < sensor_scan_count_; ++i) {
        if (strcasecmp(sensor_scan_results_[i].addr, addr) == 0) {
            sensor_scan_results_[i].rssi = rssi;
#if AC_OXIMETRY_BLE_ENABLED
            portEXIT_CRITICAL(&sensor_mux_);
#endif
            return;
        }
    }
    if (sensor_scan_count_ < AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS) {
        OximetrySensorDevice &dev = sensor_scan_results_[sensor_scan_count_++];
        strncpy(dev.addr, addr, sizeof(dev.addr) - 1);
        dev.addr[sizeof(dev.addr) - 1] = 0;
        dev.addr_type = addr_type;
        strncpy(dev.name, name ? name : "", sizeof(dev.name) - 1);
        dev.name[sizeof(dev.name) - 1] = 0;
        dev.rssi = rssi;
        dev.autoconnect = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
}

bool OximetryManager::sensor_pick_autoconnect_target(
    OximetrySensorDevice &target,
    uint32_t now_ms) {
    int best_rssi = -999;
    bool found = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    for (uint8_t i = 0; i < sensor_scan_count_; ++i) {
        const bool holdoff_active =
            sensor_auto_holdoff_until_ms_ &&
            static_cast<int32_t>(now_ms - sensor_auto_holdoff_until_ms_) < 0 &&
            (!sensor_auto_holdoff_addr_[0] ||
             strcasecmp(sensor_auto_holdoff_addr_,
                        sensor_scan_results_[i].addr) == 0);
        if (holdoff_active) continue;
        for (const auto &known : sensor_known_) {
            if (!known.addr[0] || !known.autoconnect) continue;
            if (strcasecmp(known.addr, sensor_scan_results_[i].addr) != 0) {
                continue;
            }
            if (!found || sensor_scan_results_[i].rssi > best_rssi) {
                target = sensor_scan_results_[i];
                target.autoconnect = known.autoconnect;
                best_rssi = target.rssi;
                found = true;
            }
        }
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    return found;
}

void OximetryManager::sensor_task_loop() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_set_state(OximetrySensorState::Idle);
    SensorBleScanCallbacks scan_callbacks(this);
    SensorBleClientCallbacks client_callbacks(this);
    uint32_t next_auto_scan_ms = 0;
    uint32_t last_viatom_poll_ms = 0;

    while (true) {
        bool enabled = false;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        enabled = sensor_enabled_;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        if (!enabled) {
            if (sensor_client && sensor_client->isConnected()) {
                sensor_client->disconnect();
            }
            sensor_set_state(OximetrySensorState::Off);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!ensure_ble_runtime(ble_name_)) {
            set_error("BLE init failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!sensor_client) {
            sensor_client = NimBLEDevice::createClient();
            if (sensor_client) sensor_client->setClientCallbacks(
                &client_callbacks, false);
        }
        if (!sensor_client) {
            set_error("BLE sensor client failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool disconnect_now = false;
        bool manual_scan = false;
        bool manual_connect = false;
        char manual_target[sizeof(sensor_manual_target_)] = {};
        OximetrySensorDevice manual_target_device;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        disconnect_now = sensor_disconnect_requested_;
        manual_scan = sensor_scan_requested_;
        manual_connect = sensor_manual_connect_requested_;
        strncpy(manual_target, sensor_manual_target_,
                sizeof(manual_target) - 1);
        manual_target[sizeof(manual_target) - 1] = 0;
        manual_target_device = sensor_manual_target_device_;
        sensor_disconnect_requested_ = false;
        sensor_scan_requested_ = false;
        sensor_manual_connect_requested_ = false;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif

        if (disconnect_now) {
            char holdoff_addr[sizeof(sensor_connected_addr_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
            portENTER_CRITICAL(&sensor_mux_);
#endif
            strncpy(holdoff_addr, sensor_connected_addr_,
                    sizeof(holdoff_addr) - 1);
            holdoff_addr[sizeof(holdoff_addr) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
            portEXIT_CRITICAL(&sensor_mux_);
#endif
            if (sensor_client->isConnected()) sensor_client->disconnect();
            sensor_viatom_write = nullptr;
            viatom_reset_rx();
            viatom_clear_pending();
            sensor_viatom_need_info = false;
            sensor_viatom_need_time_sync = false;
            sensor_hold_autoconnect(holdoff_addr, millis());
            sensor_invalid_since_ms_ = 0;
            sensor_set_state(OximetrySensorState::Idle);
        }

        const uint32_t now_ms = millis();
        if (sensor_viatom_pending_cmd != VIATOM_NO_PENDING_CMD &&
            static_cast<int32_t>(now_ms - sensor_viatom_pending_ms) >=
                static_cast<int32_t>(VIATOM_RESPONSE_TIMEOUT_MS)) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor Viatom response timeout cmd=%s\n",
                      viatom_cmd_name(sensor_viatom_pending_cmd));
            viatom_reset_rx();
            viatom_clear_pending();
        }

        if (sensor_client->isConnected() && sensor_viatom_write &&
            sensor_viatom_pending_cmd == VIATOM_NO_PENDING_CMD) {
            if (sensor_viatom_need_info) {
                if (viatom_send_command(sensor_viatom_write, VIATOM_CMD_INFO,
                                        nullptr, 0, now_ms)) {
                    sensor_viatom_need_info = false;
                } else {
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "[OXI] Sensor Viatom info write failed\n");
                }
            } else if (sensor_viatom_need_time_sync) {
                if (viatom_sync_datetime(sensor_viatom_write)) {
                    sensor_viatom_pending_cmd = VIATOM_CMD_CONFIG;
                    sensor_viatom_pending_ms = now_ms;
                }
                sensor_viatom_need_time_sync = false;
            } else if (static_cast<int32_t>(now_ms - last_viatom_poll_ms) >=
                       static_cast<int32_t>(VIATOM_SENSOR_POLL_MS)) {
                if (viatom_send_command(sensor_viatom_write,
                                        VIATOM_CMD_READ_SENSORS,
                                        nullptr, 0, now_ms)) {
                    last_viatom_poll_ms = now_ms;
                } else {
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "[OXI] Sensor Viatom poll write failed\n");
                    last_viatom_poll_ms = now_ms;
                }
            }
        }

        bool auto_allowed = false;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        auto_allowed = sensor_auto_allowed_;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        const bool auto_scan =
            auto_allowed &&
            has_sensor_autoconnect() &&
            !sensor_client->isConnected() &&
            static_cast<int32_t>(now_ms - next_auto_scan_ms) >= 0;

        if (!manual_scan && !manual_connect && !auto_scan) {
            if (!sensor_client->isConnected()) {
                sensor_set_state(OximetrySensorState::Idle);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (manual_connect && manual_target_device.addr[0]) {
            (void)sensor_connect_target(manual_target_device, true);
            continue;
        }

        if (sensor_client->isConnected() && (manual_scan || manual_connect)) {
            sensor_client->disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        NimBLEScan *scan = NimBLEDevice::getScan();
        if (!scan) {
            set_error("BLE scan unavailable");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        sensor_scan_count_ = 0;
        for (auto &dev : sensor_scan_results_) dev = OximetrySensorDevice{};
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        status_.sensor_scans++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        sensor_set_state(OximetrySensorState::Scanning);
        scan->clearResults();
        scan->setScanCallbacks(&scan_callbacks, false);
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(99);
        (void)scan->getResults(AC_OXIMETRY_SENSOR_SCAN_MS, false);
        next_auto_scan_ms =
            millis() + AC_OXIMETRY_SENSOR_SCAN_IDLE_MS;

        OximetrySensorDevice target;
        bool have_target = false;
        if (manual_connect) {
#if AC_OXIMETRY_BLE_ENABLED
            portENTER_CRITICAL(&sensor_mux_);
#endif
            have_target = resolve_sensor_target(manual_target, target);
#if AC_OXIMETRY_BLE_ENABLED
            portEXIT_CRITICAL(&sensor_mux_);
#endif
        } else if (auto_scan && auto_allowed) {
            have_target = sensor_pick_autoconnect_target(target, now_ms);
        }

        if (have_target) {
            (void)sensor_connect_target(target, manual_connect);
        } else {
            sensor_set_state(OximetrySensorState::Idle);
        }
    }
#endif
}

bool OximetryManager::sensor_connect_target(
    const OximetrySensorDevice &target,
    bool manual) {
#if AC_OXIMETRY_BLE_ENABLED
    if (!sensor_client || !target.addr[0]) return false;
    sensor_set_state(OximetrySensorState::Connecting);
    NimBLEDevice::getScan()->stop();
    if (sensor_client->isConnected()) sensor_client->disconnect();
        sensor_viatom_write = nullptr;
        viatom_reset_rx();
        viatom_clear_pending();
        sensor_viatom_need_info = false;
        sensor_viatom_need_time_sync = false;
        sensor_invalid_since_ms_ = 0;
        sensor_client->cancelConnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    NimBLEAddress address(std::string(target.addr), target.addr_type);
    Log::logf(CAT_OXI, LOG_INFO,
              "[OXI] Sensor connecting addr=%s type=%u name=\"%s\"\n",
              target.addr,
              static_cast<unsigned>(target.addr_type),
              target.name);
    if (!sensor_client->connect(address)) {
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        status_.sensor_connect_failures++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor connect failed addr=%s err=%d\n",
                  target.addr,
                  sensor_client->getLastError());
        sensor_set_state(OximetrySensorState::Idle);
        return false;
    }

    const bool needs_encryption = strncmp(target.name, "Nonin", 5) == 0;
    if (needs_encryption && !sensor_client->secureConnection(false)) {
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        status_.sensor_connect_failures++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor encryption failed addr=%s err=%d\n",
                  target.addr,
                  sensor_client->getLastError());
        sensor_client->disconnect();
        sensor_set_state(OximetrySensorState::Idle);
        return false;
    }

    if (!sensor_subscribe_client(sensor_client, target.name)) {
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        status_.sensor_connect_failures++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor has no supported oximetry service addr=%s\n",
                  target.addr);
        sensor_client->disconnect();
        sensor_set_state(OximetrySensorState::Idle);
        return false;
    }

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    strncpy(sensor_connected_addr_, target.addr,
            sizeof(sensor_connected_addr_) - 1);
    sensor_connected_addr_[sizeof(sensor_connected_addr_) - 1] = 0;
    strncpy(sensor_connected_name_, target.name,
            sizeof(sensor_connected_name_) - 1);
    sensor_connected_name_[sizeof(sensor_connected_name_) - 1] = 0;
    if (manual) {
        size_t known_index = 0;
        if (!find_sensor_addr(target.addr, known_index)) {
            for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
                if (sensor_known_[i].addr[0]) continue;
                sensor_known_[i] = target;
                sensor_known_[i].autoconnect = true;
                break;
            }
        } else {
            sensor_known_[known_index].addr_type = target.addr_type;
            strncpy(sensor_known_[known_index].name, target.name,
                    sizeof(sensor_known_[known_index].name) - 1);
        }
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    if (manual) save_sensor_known();
    if (sensor_viatom_write) {
        viatom_reset_rx();
        viatom_clear_pending();
        sensor_viatom_need_info = true;
        sensor_viatom_need_time_sync = true;
    }

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    status_.sensor_connects++;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    sensor_set_state(OximetrySensorState::Connected);
    Log::logf(CAT_OXI, LOG_INFO,
              "[OXI] Sensor connected addr=%s name=\"%s\"\n",
              target.addr, target.name);
    return true;
#else
    (void)target;
    (void)manual;
    return false;
#endif
}

bool OximetryManager::sensor_subscribe_client(void *client_ptr,
                                              const char *name) {
#if AC_OXIMETRY_BLE_ENABLED
    auto *client = static_cast<NimBLEClient *>(client_ptr);
    if (!client) return false;
    bool subscribed = false;
    sensor_viatom_write = nullptr;

    NimBLERemoteService *plx_service =
        client->getService(NimBLEUUID(PLX_SERVICE_UUID));
    if (plx_service) {
        NimBLERemoteCharacteristic *continuous =
            plx_service->getCharacteristic(NimBLEUUID(PLX_CONTINUOUS_UUID));
        if (continuous && continuous->canNotify() &&
            continuous->subscribe(true, sensor_plx_notify_cb)) {
            subscribed = true;
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor subscribed PLX continuous\n");
        }
        if (!subscribed) {
            NimBLERemoteCharacteristic *spot =
                plx_service->getCharacteristic(NimBLEUUID(PLX_SPOT_UUID));
            if (spot && (spot->canNotify() || spot->canIndicate()) &&
                spot->subscribe(spot->canNotify(),
                                sensor_plx_notify_cb)) {
                subscribed = true;
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "[OXI] Sensor subscribed PLX spot\n");
            }
        }
    }

    if (!subscribed) {
        NimBLERemoteService *nonin_service =
            client->getService(NimBLEUUID(NONIN_SERVICE_UUID));
        if (nonin_service) {
            NimBLERemoteCharacteristic *continuous =
                nonin_service->getCharacteristic(
                    NimBLEUUID(NONIN_CONTINUOUS_UUID));
            if (continuous && continuous->canNotify() &&
                continuous->subscribe(true, sensor_nonin_notify_cb)) {
                subscribed = true;
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "[OXI] Sensor subscribed Nonin continuous\n");
            }
        }
    }

    if (!subscribed) {
        NimBLERemoteService *viatom_service =
            client->getService(NimBLEUUID(VIATOM_SERVICE_UUID));
        if (viatom_service) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor Viatom service found\n");
            NimBLERemoteCharacteristic *read =
                viatom_service->getCharacteristic(
                    NimBLEUUID(VIATOM_READ_UUID));
            if (read && read->canNotify() &&
                read->subscribe(true, sensor_viatom_notify_cb)) {
                sensor_viatom_write =
                    viatom_service->getCharacteristic(
                        NimBLEUUID(VIATOM_WRITE_UUID));
                subscribed = sensor_viatom_write != nullptr;
                if (subscribed) {
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "[OXI] Sensor subscribed Viatom read\n");
                } else {
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "[OXI] Sensor Viatom write missing\n");
                }
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "[OXI] Sensor Viatom read subscribe failed\n");
            }
        }
    }

    (void)name;
    return subscribed;
#else
    (void)client_ptr;
    (void)name;
    return false;
#endif
}

bool OximetryManager::ensure_udp(bool network_available) {
    if (!status_.enabled || !network_available) {
        stop_udp();
        return false;
    }
    if (status_.udp_started) return true;
    if (!udp_.begin(status_.udp_port)) {
        set_error("UDP bind failed");
        return false;
    }
    status_.udp_started = true;
    Log::logf(CAT_OXI, LOG_INFO, "[OXI] UDP listening on port %u\n",
              status_.udp_port);
    return true;
}

void OximetryManager::stop_udp() {
    if (!status_.udp_started) return;
    udp_.stop();
    status_.udp_started = false;
}

void OximetryManager::poll_udp(uint32_t now_ms) {
    if (!status_.udp_started) return;
    for (size_t i = 0; i < AC_OXIMETRY_UDP_READ_BUDGET; ++i) {
        const int packet_size = udp_.parsePacket();
        if (packet_size <= 0) return;
        const IPAddress remote_ip = udp_.remoteIP();
        uint8_t packet[AC_OXIMETRY_UDP_PACKET_SIZE] = {};
        const int read = udp_.read(packet, sizeof(packet));
        while (udp_.available()) udp_.read();

        if (packet_size != static_cast<int>(AC_OXIMETRY_UDP_PACKET_SIZE) ||
            read != static_cast<int>(AC_OXIMETRY_UDP_PACKET_SIZE) ||
            packet[0] != 0x55 || packet[1] != 0xab) {
            status_.udp_bad_packets++;
            continue;
        }

        const uint16_t spo2_raw =
            static_cast<uint16_t>(packet[3]) |
            (static_cast<uint16_t>(packet[4]) << 8);
        const uint16_t pulse_raw =
            static_cast<uint16_t>(packet[5]) |
            (static_cast<uint16_t>(packet[6]) << 8);
        note_udp_packet(spo2_raw, pulse_raw, remote_ip, now_ms);
    }
}

void OximetryManager::note_udp_packet(uint16_t spo2_raw,
                                      uint16_t pulse_raw,
                                      IPAddress remote_ip,
                                      uint32_t now_ms) {
    char source_detail[48];
    snprintf(source_detail, sizeof(source_detail),
             "%u.%u.%u.%u",
             remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3]);
    (void)note_source_packet(OximetrySource::Udp, source_detail, spo2_raw,
                             pulse_raw, false, now_ms);
}

bool OximetryManager::note_source_packet(OximetrySource source,
                                         const char *detail,
                                         uint16_t spo2_raw,
                                         uint16_t pulse_raw,
                                         bool allow_invalid_claim,
                                         uint32_t now_ms) {
    bool spo2_valid = false;
    bool pulse_valid = false;
    const int16_t spo2 = decode_plx_sfloat(spo2_raw, spo2_valid);
    const int16_t pulse = decode_plx_sfloat(pulse_raw, pulse_valid);
    const bool valid = spo2_valid && pulse_valid;

    if (source_present_ && source_ != source) return false;
    if (!source_present_ && !valid && !allow_invalid_claim) return false;

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
                char holdoff_addr[sizeof(sensor_connected_addr_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
                portENTER_CRITICAL(&sensor_mux_);
#endif
                strncpy(holdoff_addr, sensor_connected_addr_,
                        sizeof(holdoff_addr) - 1);
                holdoff_addr[sizeof(holdoff_addr) - 1] = 0;
                sensor_disconnect_requested_ = true;
#if AC_OXIMETRY_BLE_ENABLED
                portEXIT_CRITICAL(&sensor_mux_);
#endif
                sensor_hold_autoconnect(holdoff_addr, now_ms);
                sensor_invalid_since_ms_ = now_ms;
                Log::logf(CAT_OXI, LOG_INFO,
                          "[OXI] Sensor invalid readings; disconnecting\n");
            }
        }
    }
    return true;
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

void OximetryManager::drain_sensor_events(uint32_t now_ms) {
    bool sample_pending = false;
    uint16_t spo2_raw = PLX_SFLOAT_NAN;
    uint16_t pulse_raw = PLX_SFLOAT_NAN;
    bool invalid_packet = false;
    bool disconnect_pending = false;
    int disconnect_reason = 0;

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sample_pending = sensor_sample_pending_;
    spo2_raw = sensor_pending_spo2_raw_;
    pulse_raw = sensor_pending_pulse_raw_;
    invalid_packet = sensor_pending_invalid_packet_;
    disconnect_pending = sensor_disconnect_pending_;
    disconnect_reason = sensor_pending_disconnect_reason_;
    sensor_sample_pending_ = false;
    sensor_disconnect_pending_ = false;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif

    if (sample_pending) {
        (void)invalid_packet;
        char detail[64] = {};
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        if (sensor_connected_name_[0]) {
            snprintf(detail, sizeof(detail), "%s %s",
                     sensor_connected_addr_, sensor_connected_name_);
        } else {
            strncpy(detail, sensor_connected_addr_, sizeof(detail) - 1);
            detail[sizeof(detail) - 1] = 0;
        }
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        if (note_source_packet(OximetrySource::Ble, detail, spo2_raw,
                               pulse_raw, true, now_ms)) {
            sensor_set_state(OximetrySensorState::Streaming);
        }
    }

    if (disconnect_pending) {
        if (source_ == OximetrySource::Ble) {
            source_present_ = false;
            source_ = OximetrySource::None;
            reading_ = OximetryReading{};
            reading_.spo2 = -1;
            reading_.pulse_bpm = -1;
            status_.source_detail[0] = 0;
            sensor_invalid_since_ms_ = 0;
        }
        status_.sensor_disconnects++;
        Log::logf(CAT_OXI, LOG_INFO,
                  "[OXI] Sensor disconnected reason=%d\n",
                  disconnect_reason);
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

void OximetryManager::mark_source_stale(uint32_t now_ms) {
    (void)now_ms;
    const OximetrySource stale_source = source_;
    source_present_ = false;
    source_ = OximetrySource::None;
    reading_.valid = false;
    reading_.spo2 = -1;
    reading_.pulse_bpm = -1;
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
    valid = false;
    if (raw == PLX_SFLOAT_NAN || raw == PLX_SFLOAT_NRES ||
        raw == PLX_SFLOAT_POS_INF || raw == PLX_SFLOAT_NEG_INF ||
        raw == PLX_SFLOAT_RESERVED) {
        return -1;
    }

    int16_t mantissa = raw & 0x0fff;
    if (mantissa & 0x0800) mantissa |= 0xf000;
    int8_t exponent = static_cast<int8_t>((raw >> 12) & 0x0f);
    if (exponent & 0x08) exponent |= 0xf0;

    float value = static_cast<float>(mantissa);
    while (exponent > 0) {
        value *= 10.0f;
        exponent--;
    }
    while (exponent < 0) {
        value /= 10.0f;
        exponent++;
    }
    if (value < 0.0f || value > 300.0f) return -1;
    valid = true;
    return static_cast<int16_t>(value + 0.5f);
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

void OximetryManager::on_sensor_sample(uint16_t spo2_raw,
                                       uint16_t pulse_raw,
                                       bool from_invalid_packet) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_pending_spo2_raw_ = spo2_raw;
    sensor_pending_pulse_raw_ = pulse_raw;
    sensor_pending_invalid_packet_ = from_invalid_packet;
    sensor_sample_pending_ = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
}

void OximetryManager::on_sensor_disconnect(int reason) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_disconnect_pending_ = true;
    sensor_pending_disconnect_reason_ = reason;
    sensor_connected_addr_[0] = 0;
    sensor_connected_name_[0] = 0;
    status_.sensor_connected = false;
    status_.sensor_state = OximetrySensorState::Idle;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
}

}  // namespace aircannect
