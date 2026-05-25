#include "oximetry_internal.h"
#include "oximetry_sensor_protocols.h"

#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "string_util.h"

namespace aircannect {

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

class SensorBleScanCallbacks : public NimBLEScanCallbacks {
public:
    explicit SensorBleScanCallbacks(OximetryManager *owner)
        : owner_(owner) {}

    void onResult(const NimBLEAdvertisedDevice *dev) override {
        if (!owner_ || !dev) return;
        if (!sensor_matches_supported_device(dev)) return;
        const std::string name = dev->getName();

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
        sensor_protocols_reset();
        if (owner_) {
            owner_->sensor_invalid_since_ms_ = 0;
            owner_->on_sensor_disconnect(reason);
        }
    }

    bool onConnParamsUpdateRequest(NimBLEClient *client,
                                   const ble_gap_upd_params *params) override {
        (void)client;
        if (params) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor conn params request min=%u max=%u latency=%u timeout=%u\n",
                      static_cast<unsigned>(params->itvl_min),
                      static_cast<unsigned>(params->itvl_max),
                      static_cast<unsigned>(params->latency),
                      static_cast<unsigned>(params->supervision_timeout));
        }
        return true;
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
    if (!status_.enabled) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor scan ignored: oximetry disabled\n");
        return false;
    }
    ensure_sensor_task();
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_scan_requested_ = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    Log::logf(CAT_OXI, LOG_INFO, "[OXI] Sensor scan queued\n");
    return true;
}

bool OximetryManager::request_sensor_connect(const char *addr_or_index) {
    if (!status_.enabled || !addr_or_index || !addr_or_index[0]) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor connect ignored: invalid request\n");
        return false;
    }
    OximetrySensorDevice target;
    uint8_t scan_count = 0;
    uint8_t known_count = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    const bool ok = resolve_sensor_target(addr_or_index, target);
    scan_count = sensor_scan_count_;
    for (const auto &known : sensor_known_) {
        if (known.addr[0]) known_count++;
    }
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
    if (ok) {
        ensure_sensor_task();
        Log::logf(CAT_OXI, LOG_INFO,
                  "[OXI] Sensor connect queued target=\"%s\" addr=%s type=%u name=\"%s\"\n",
                  addr_or_index,
                  target.addr,
                  static_cast<unsigned>(target.addr_type),
                  target.name);
    } else {
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor connect target not found target=\"%s\" scan=%u known=%u\n",
                  addr_or_index,
                  static_cast<unsigned>(scan_count),
                  static_cast<unsigned>(known_count));
    }
    return ok;
}

bool OximetryManager::request_sensor_connect_device(
    const OximetrySensorDevice &device) {
    if (!status_.enabled || !device.addr[0]) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "[OXI] Sensor connect ignored: invalid device request\n");
        return false;
    }
    OximetrySensorDevice target = device;
    OximetrySensorDevice resolved;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    if (resolve_sensor_target(device.addr, resolved)) {
        if (!target.name[0]) {
            strncpy(target.name, resolved.name, sizeof(target.name) - 1);
            target.name[sizeof(target.name) - 1] = 0;
        }
        if (!target.rssi) target.rssi = resolved.rssi;
        target.addr_type = resolved.addr_type;
    }
    strncpy(sensor_manual_target_, target.addr,
            sizeof(sensor_manual_target_) - 1);
    sensor_manual_target_[sizeof(sensor_manual_target_) - 1] = 0;
    sensor_manual_target_device_ = target;
    sensor_manual_connect_requested_ = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    ensure_sensor_task();
    Log::logf(CAT_OXI, LOG_INFO,
              "[OXI] Sensor connect queued addr=%s type=%u name=\"%s\"\n",
              target.addr,
              static_cast<unsigned>(target.addr_type),
              target.name);
    return true;
}

bool OximetryManager::request_sensor_disconnect() {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_disconnect_requested_ = true;
    sensor_disconnect_hold_until_absent_ = true;
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
                                              uint32_t now_ms,
                                              bool until_absent) {
    bool changed = true;
    char logged_addr[sizeof(sensor_auto_holdoff_addr_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    const char *hold_addr = addr ? addr : "";
    const bool same_addr =
        strcasecmp(sensor_auto_holdoff_addr_, hold_addr) == 0;
    const bool timed_hold_active =
        sensor_auto_holdoff_until_ms_ &&
        static_cast<int32_t>(now_ms - sensor_auto_holdoff_until_ms_) < 0;
    if (same_addr && sensor_auto_holdoff_until_absent_ == until_absent &&
        timed_hold_active) {
        changed = false;
    } else {
        sensor_auto_holdoff_until_absent_ = until_absent;
        sensor_auto_holdoff_until_ms_ =
            now_ms + AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS;
        strncpy(sensor_auto_holdoff_addr_, hold_addr,
                sizeof(sensor_auto_holdoff_addr_) - 1);
        sensor_auto_holdoff_addr_[sizeof(sensor_auto_holdoff_addr_) - 1] = 0;
    }
    strncpy(logged_addr, sensor_auto_holdoff_addr_, sizeof(logged_addr) - 1);
    logged_addr[sizeof(logged_addr) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    if (!changed) return;
    if (until_absent) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor auto-connect holdoff addr=%s until=absent max_ms=%lu\n",
                  logged_addr[0] ? logged_addr : "*",
                  static_cast<unsigned long>(
                      AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS));
    } else {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor auto-connect holdoff addr=%s ms=%lu\n",
                  logged_addr[0] ? logged_addr : "*",
                  static_cast<unsigned long>(
                      AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS));
    }
}

bool OximetryManager::sensor_autoconnect_holdoff_active(
    uint32_t now_ms) const {
    bool active = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    if (sensor_auto_holdoff_addr_[0]) {
        active = sensor_auto_holdoff_until_ms_ &&
                 static_cast<int32_t>(
                     now_ms - sensor_auto_holdoff_until_ms_) < 0;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&sensor_mux_));
#endif
    return active;
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
    bool holdoff_cleared = false;
    char holdoff_cleared_addr[sizeof(sensor_auto_holdoff_addr_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    bool holdoff_seen = false;
    bool timed_hold_active =
        sensor_auto_holdoff_until_ms_ &&
        static_cast<int32_t>(now_ms - sensor_auto_holdoff_until_ms_) < 0;
    if (!timed_hold_active) {
        sensor_auto_holdoff_addr_[0] = 0;
        sensor_auto_holdoff_until_ms_ = 0;
        sensor_auto_holdoff_until_absent_ = false;
    }
    for (uint8_t i = 0; i < sensor_scan_count_; ++i) {
        const bool matches_holdoff =
            sensor_auto_holdoff_addr_[0] &&
            strcasecmp(sensor_auto_holdoff_addr_,
                       sensor_scan_results_[i].addr) == 0;
        if (matches_holdoff) holdoff_seen = true;
        const bool holdoff_active =
            matches_holdoff && timed_hold_active;
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
    if (sensor_auto_holdoff_until_absent_ &&
        sensor_auto_holdoff_addr_[0] && !holdoff_seen) {
        strncpy(holdoff_cleared_addr, sensor_auto_holdoff_addr_,
                sizeof(holdoff_cleared_addr) - 1);
        holdoff_cleared_addr[sizeof(holdoff_cleared_addr) - 1] = 0;
        sensor_auto_holdoff_addr_[0] = 0;
        sensor_auto_holdoff_until_ms_ = 0;
        sensor_auto_holdoff_until_absent_ = false;
        holdoff_cleared = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
    if (holdoff_cleared) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor auto-connect holdoff cleared addr=%s\n",
                  holdoff_cleared_addr);
    }
    return found;
}

void OximetryManager::sensor_task_loop() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_set_state(OximetrySensorState::Idle);
    SensorBleScanCallbacks scan_callbacks(this);
    SensorBleClientCallbacks client_callbacks(this);
    uint32_t next_auto_scan_ms = 0;

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
            if (sensor_client) {
                sensor_client->setClientCallbacks(&client_callbacks, false);
                sensor_client->setConnectionParams(
                    AC_OXIMETRY_SENSOR_CONN_INTERVAL_MIN,
                    AC_OXIMETRY_SENSOR_CONN_INTERVAL_MAX,
                    AC_OXIMETRY_SENSOR_CONN_LATENCY,
                    AC_OXIMETRY_SENSOR_CONN_TIMEOUT);
            }
        }
        if (!sensor_client) {
            set_error("BLE sensor client failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool disconnect_now = false;
        bool disconnect_hold_until_absent = false;
        bool manual_scan = false;
        bool manual_connect = false;
        char manual_target[sizeof(sensor_manual_target_)] = {};
        OximetrySensorDevice manual_target_device;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        disconnect_now = sensor_disconnect_requested_;
        disconnect_hold_until_absent = sensor_disconnect_hold_until_absent_;
        manual_scan = sensor_scan_requested_;
        manual_connect = sensor_manual_connect_requested_;
        strncpy(manual_target, sensor_manual_target_,
                sizeof(manual_target) - 1);
        manual_target[sizeof(manual_target) - 1] = 0;
        manual_target_device = sensor_manual_target_device_;
        sensor_disconnect_requested_ = false;
        sensor_disconnect_hold_until_absent_ = false;
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
            sensor_protocols_reset();
            sensor_hold_autoconnect(holdoff_addr, millis(),
                                    disconnect_hold_until_absent);
            sensor_invalid_since_ms_ = 0;
            sensor_set_state(OximetrySensorState::Idle);
        }

        const uint32_t now_ms = millis();
        sensor_protocols_poll(now_ms);

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
            Log::logf(CAT_OXI, LOG_INFO,
                      "[OXI] Sensor manual connect starting addr=%s type=%u name=\"%s\"\n",
                      manual_target_device.addr,
                      static_cast<unsigned>(manual_target_device.addr_type),
                      manual_target_device.name);
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
        const log_level_t scan_log_level =
            (manual_scan || manual_connect) ? LOG_INFO : LOG_DEBUG;
        Log::logf(CAT_OXI, scan_log_level,
                  "[OXI] Sensor scan started manual=%s connect=%s auto=%s\n",
                  manual_scan ? "yes" : "no",
                  manual_connect ? "yes" : "no",
                  auto_scan ? "yes" : "no");
        scan->clearResults();
        scan->setScanCallbacks(&scan_callbacks, false);
        scan->setMaxResults(0);
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(99);
        (void)scan->getResults(AC_OXIMETRY_SENSOR_SCAN_MS, false);
        scan->clearResults();
        next_auto_scan_ms =
            millis() + AC_OXIMETRY_SENSOR_SCAN_IDLE_MS;

        OximetrySensorDevice scan_log[AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
        size_t scan_log_count = 0;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&sensor_mux_);
#endif
        scan_log_count = sensor_scan_count_;
        if (scan_log_count > AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS) {
            scan_log_count = AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS;
        }
        for (size_t i = 0; i < scan_log_count; ++i) {
            scan_log[i] = sensor_scan_results_[i];
        }
        sensor_scan_generation_++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&sensor_mux_);
#endif
        Log::logf(CAT_OXI, scan_log_level,
                  "[OXI] Sensor scan complete count=%u\n",
                  static_cast<unsigned>(scan_log_count));
        for (size_t i = 0; i < scan_log_count; ++i) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor scan result %u addr=%s type=%u rssi=%d name=\"%s\"\n",
                      static_cast<unsigned>(i),
                      scan_log[i].addr,
                      static_cast<unsigned>(scan_log[i].addr_type),
                      scan_log[i].rssi,
                      scan_log[i].name);
        }

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
            if (manual_connect) {
                Log::logf(CAT_OXI, LOG_WARN,
                          "[OXI] Sensor manual connect target not found target=%s count=%u\n",
                          manual_target[0] ? manual_target : "--",
                          static_cast<unsigned>(scan_log_count));
            } else if (auto_scan) {
                if (sensor_autoconnect_holdoff_active(millis())) {
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "[OXI] Sensor auto-connect holdoff active\n");
                }
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "[OXI] Sensor auto scan found no autoconnect target count=%u\n",
                          static_cast<unsigned>(scan_log_count));
            }
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
    sensor_protocols_reset();
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
    sensor_protocols_on_connected();

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
    sensor_protocols_reset();

    (void)name;
    return sensor_subscribe_supported_device(client);
#else
    (void)client_ptr;
    (void)name;
    return false;
#endif
}

void OximetryManager::drain_sensor_events(uint32_t now_ms) {
    bool sample_pending = false;
    uint16_t spo2_raw = PLX_SFLOAT_NAN;
    uint16_t pulse_raw = PLX_SFLOAT_NAN;
    bool invalid_packet = false;
    bool contact_known = false;
    bool contact_present = false;
    bool disconnect_pending = false;
    int disconnect_reason = 0;
    char disconnect_addr[sizeof(sensor_pending_disconnect_addr_)] = {};

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sample_pending = sensor_sample_pending_;
    spo2_raw = sensor_pending_spo2_raw_;
    pulse_raw = sensor_pending_pulse_raw_;
    invalid_packet = sensor_pending_invalid_packet_;
    contact_known = sensor_pending_contact_known_;
    contact_present = sensor_pending_contact_present_;
    disconnect_pending = sensor_disconnect_pending_;
    disconnect_reason = sensor_pending_disconnect_reason_;
    strncpy(disconnect_addr, sensor_pending_disconnect_addr_,
            sizeof(disconnect_addr) - 1);
    disconnect_addr[sizeof(disconnect_addr) - 1] = 0;
    sensor_sample_pending_ = false;
    sensor_pending_contact_known_ = false;
    sensor_pending_contact_present_ = false;
    sensor_disconnect_pending_ = false;
    sensor_pending_disconnect_addr_[0] = 0;
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
                               pulse_raw, true, contact_known,
                               contact_present, now_ms)) {
            sensor_set_state(OximetrySensorState::Streaming);
        }
    }

    if (disconnect_pending) {
        const bool disconnected_after_invalid =
            source_ == OximetrySource::Ble && source_present_ &&
            !reading_.valid;
        if (disconnect_addr[0] && disconnected_after_invalid) {
            sensor_hold_autoconnect(disconnect_addr, now_ms, true);
        }
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
                  "[OXI] Sensor disconnected addr=%s reason=%d\n",
                  disconnect_addr[0] ? disconnect_addr : "--",
                  disconnect_reason);
    }
}

void OximetryManager::on_sensor_sample(uint16_t spo2_raw,
                                       uint16_t pulse_raw,
                                       bool from_invalid_packet,
                                       bool contact_known,
                                       bool contact_present) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&sensor_mux_);
#endif
    sensor_pending_spo2_raw_ = spo2_raw;
    sensor_pending_pulse_raw_ = pulse_raw;
    sensor_pending_invalid_packet_ = from_invalid_packet;
    sensor_pending_contact_known_ = contact_known;
    sensor_pending_contact_present_ = contact_present;
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
    strncpy(sensor_pending_disconnect_addr_, sensor_connected_addr_,
            sizeof(sensor_pending_disconnect_addr_) - 1);
    sensor_pending_disconnect_addr_[
        sizeof(sensor_pending_disconnect_addr_) - 1] = 0;
    sensor_connected_addr_[0] = 0;
    sensor_connected_name_[0] = 0;
    status_.sensor_connected = false;
    status_.sensor_state = OximetrySensorState::Idle;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&sensor_mux_);
#endif
}

}  // namespace aircannect
