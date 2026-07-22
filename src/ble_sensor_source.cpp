#include "ble_sensor_source.h"

#include <Preferences.h>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "oximetry_codec.h"
#include "string_util.h"

namespace aircannect {

namespace {

static constexpr const char *SENSOR_NS = "oxi_sensor";
static constexpr const char *SENSOR_KNOWN_COUNT_KEY = "known_count";

void known_key(char *out, size_t out_len, size_t index, const char *field) {
    snprintf(out, out_len, "known%u_%s",
             static_cast<unsigned>(index),
             field);
}

void remove_nvs_key_if_present(Preferences &prefs, const char *key) {
    if (prefs.isKey(key)) prefs.remove(key);
}

}  // namespace

class SensorBleScanCallbacks : public NimBLEScanCallbacks {
public:
    explicit SensorBleScanCallbacks(BleSensorSource *owner)
        : owner_(owner) {}

    void onResult(const NimBLEAdvertisedDevice *dev) override {
        if (!owner_ || !dev) return;
        if (!owner_->protocols_.matches(dev)) return;
        const std::string name = dev->getName();

        owner_->store_scan_result(
            dev->getAddress().toString().c_str(),
            dev->getAddress().getType(),
            name.c_str(),
            dev->getRSSI());
    }

private:
    BleSensorSource *owner_ = nullptr;
};

class SensorBleClientCallbacks : public NimBLEClientCallbacks {
public:
    explicit SensorBleClientCallbacks(BleSensorSource *owner)
        : owner_(owner) {}

    void onDisconnect(NimBLEClient *client, int reason) override {
        (void)client;
        if (owner_) {
            owner_->protocols_.reset();
            owner_->callback_disconnected(reason);
        }
    }

    bool onConnParamsUpdateRequest(NimBLEClient *client,
                                   const ble_gap_upd_params *params) override {
        (void)client;
        if (params) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor conn params request min=%u max=%u latency=%u timeout=%u\n",
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
    BleSensorSource *owner_ = nullptr;
};

bool BleSensorSource::begin(bool enabled, const char *runtime_name) {
    protocols_.set_sample_callback(protocol_sample_callback, this);
    load_known();
    configure(enabled, runtime_name);
    return true;
}

void BleSensorSource::configure(bool enabled, const char *runtime_name) {
    bool should_start = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    enabled_ = enabled;
    strncpy(runtime_name_, runtime_name && runtime_name[0]
                               ? runtime_name
                               : "aircannect",
            sizeof(runtime_name_) - 1);
    runtime_name_[sizeof(runtime_name_) - 1] = 0;
    if (!enabled_) {
        disconnect_requested_ = true;
        disconnect_hold_until_absent_ = false;
        auto_allowed_ = false;
        status_.state = OximetrySensorState::Off;
        status_.scanning = false;
    }
    should_start = enabled_;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif

    if (should_start && has_autoconnect()) ensure_task();
}

void BleSensorSource::set_auto_allowed(bool allowed) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    auto_allowed_ = allowed;
    const bool should_start = enabled_ && allowed;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif

    if (should_start && has_autoconnect()) ensure_task();
}

BleSensorStatus BleSensorSource::status() const {
    BleSensorStatus out;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    out = status_;
    out.task_started = task_started_;
    out.scan_count = scan_count_;
    out.scan_generation = scan_generation_;
    out.known_count = 0;
    for (const auto &device : known_) {
        if (device.addr[0]) out.known_count++;
    }
    strncpy(out.peer, connected_addr_, sizeof(out.peer) - 1);
    out.peer[sizeof(out.peer) - 1] = 0;
    strncpy(out.name, connected_name_, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = 0;
#if AC_STACK_PROFILE_ENABLED
    TaskHandle_t task = task_;
#endif
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
#if AC_STACK_PROFILE_ENABLED
    if (task) out.task_stack_high_water_bytes = uxTaskGetStackHighWaterMark(task);
#endif
    return out;
}

#if AC_STACK_PROFILE_ENABLED
uint32_t BleSensorSource::task_stack_high_water_bytes() const {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
    TaskHandle_t task = task_;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
    return task ? uxTaskGetStackHighWaterMark(task) : 0;
#else
    return 0;
#endif
}
#endif

size_t BleSensorSource::scan_results(OximetrySensorDevice *out, size_t max) const {
    if (!out || !max) return 0;
    size_t count = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    count = scan_count_;
    if (count > max) count = max;
    for (size_t i = 0; i < count; ++i) out[i] = scan_results_[i];
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    return count;
}

size_t BleSensorSource::known_sensors(OximetrySensorDevice *out, size_t max) const {
    if (!out || !max) return 0;
    size_t count = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    for (size_t i = 0;
         i < AC_OXIMETRY_SENSOR_MAX_KNOWN && count < max;
         ++i) {
        if (!known_[i].addr[0]) continue;
        out[count++] = known_[i];
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    return count;
}

bool BleSensorSource::load_known() {
    if (known_loaded_) return true;
    for (auto &dev : known_) dev = OximetrySensorDevice{};

    Preferences prefs;
    if (!prefs.begin(SENSOR_NS, true)) {
        known_loaded_ = true;
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
        known_key(key, sizeof(key), i, "addr");
        bool has_key = prefs.isKey(key);
        String addr = prefs.getString(key, "");
        if (!has_key) {
            snprintf(key, sizeof(key), "a%u", i);
            addr = prefs.getString(key, "");
        }
        addr.trim();
        if (!addr.length() || addr.length() >= sizeof(known_[i].addr)) {
            continue;
        }
        strncpy(known_[i].addr, addr.c_str(),
                sizeof(known_[i].addr) - 1);

        known_key(key, sizeof(key), i, "type");
        has_key = prefs.isKey(key);
        known_[i].addr_type = prefs.getUChar(key, 1);
        if (!has_key) {
            snprintf(key, sizeof(key), "t%u", i);
            known_[i].addr_type = prefs.getUChar(key, 1);
        }
        known_key(key, sizeof(key), i, "auto");
        has_key = prefs.isKey(key);
        known_[i].autoconnect = prefs.getBool(key, true);
        if (!has_key) {
            snprintf(key, sizeof(key), "e%u", i);
            known_[i].autoconnect = prefs.getBool(key, true);
        }
        known_key(key, sizeof(key), i, "name");
        has_key = prefs.isKey(key);
        String name = prefs.getString(key, "");
        if (!has_key) {
            snprintf(key, sizeof(key), "n%u", i);
            name = prefs.getString(key, "");
        }
        strncpy(known_[i].name, name.c_str(),
                sizeof(known_[i].name) - 1);
    }
    prefs.end();
    known_loaded_ = true;
    return true;
}

bool BleSensorSource::save_known() const {
    OximetrySensorDevice snapshot[AC_OXIMETRY_SENSOR_MAX_KNOWN];
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        snapshot[i] = known_[i];
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
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
        known_key(key, sizeof(key), out_index, "addr");
        ok = prefs.putString(key, snapshot[i].addr) != 0 && ok;
        known_key(key, sizeof(key), out_index, "type");
        ok = prefs.putUChar(key, snapshot[i].addr_type) != 0 && ok;
        known_key(key, sizeof(key), out_index, "auto");
        ok = prefs.putBool(key, snapshot[i].autoconnect) != 0 && ok;
        known_key(key, sizeof(key), out_index, "name");
        ok = prefs.putString(key, snapshot[i].name) != 0 && ok;
        out_index++;
    }
    for (; out_index < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++out_index) {
        char key[16];
        known_key(key, sizeof(key), out_index, "addr");
        remove_nvs_key_if_present(prefs, key);
        known_key(key, sizeof(key), out_index, "type");
        remove_nvs_key_if_present(prefs, key);
        known_key(key, sizeof(key), out_index, "auto");
        remove_nvs_key_if_present(prefs, key);
        known_key(key, sizeof(key), out_index, "name");
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

bool BleSensorSource::has_autoconnect() const {
    bool found = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    for (const auto &dev : known_) {
        if (dev.addr[0] && dev.autoconnect) {
            found = true;
            break;
        }
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    return found;
}

bool BleSensorSource::find_addr(const char *addr, size_t &index) const {
    if (!addr || !addr[0]) return false;
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        if (known_[i].addr[0] &&
            strcasecmp(known_[i].addr, addr) == 0) {
            index = i;
            return true;
        }
    }
    return false;
}

bool BleSensorSource::resolve_target(
    const char *addr_or_index,
    OximetrySensorDevice &target) const {
    if (!addr_or_index || !addr_or_index[0]) return false;

    char *end = nullptr;
    const unsigned long parsed = strtoul(addr_or_index, &end, 10);
    if (end && *end == 0) {
        if (parsed >= scan_count_) return false;
        target = scan_results_[parsed];
        return target.addr[0] != 0;
    }

    for (uint8_t i = 0; i < scan_count_; ++i) {
        if (strcasecmp(scan_results_[i].addr, addr_or_index) == 0) {
            target = scan_results_[i];
            return true;
        }
    }
    size_t known_index = 0;
    if (find_addr(addr_or_index, known_index)) {
        target = known_[known_index];
        return true;
    }
    return false;
}

bool BleSensorSource::request_scan() {
    bool enabled = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    enabled = enabled_;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (!enabled) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor scan ignored: oximetry disabled\n");
        return false;
    }
    ensure_task();
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    scan_requested_ = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    Log::logf(CAT_OXI, LOG_INFO, "Sensor scan queued\n");
    return true;
}

bool BleSensorSource::request_connect(const char *addr_or_index) {
    bool enabled = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    enabled = enabled_;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (!enabled || !addr_or_index || !addr_or_index[0]) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor connect ignored: invalid request\n");
        return false;
    }
    OximetrySensorDevice target;
    uint8_t scan_count = 0;
    uint8_t known_count = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    const bool ok = resolve_target(addr_or_index, target);
    scan_count = scan_count_;
    for (const auto &known : known_) {
        if (known.addr[0]) known_count++;
    }
    if (ok) {
        strncpy(manual_target_, target.addr,
                sizeof(manual_target_) - 1);
        manual_target_[sizeof(manual_target_) - 1] = 0;
        manual_target_device_ = target;
        manual_connect_requested_ = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (ok) {
        ensure_task();
        Log::logf(CAT_OXI, LOG_INFO,
                  "Sensor connect queued target=\"%s\" addr=%s type=%u name=\"%s\"\n",
                  addr_or_index,
                  target.addr,
                  static_cast<unsigned>(target.addr_type),
                  target.name);
    } else {
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor connect target not found target=\"%s\" scan=%u known=%u\n",
                  addr_or_index,
                  static_cast<unsigned>(scan_count),
                  static_cast<unsigned>(known_count));
    }
    return ok;
}

bool BleSensorSource::request_connect(const OximetrySensorDevice &device) {
    bool enabled = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    enabled = enabled_;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (!enabled || !device.addr[0]) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor connect ignored: invalid device request\n");
        return false;
    }
    OximetrySensorDevice target = device;
    OximetrySensorDevice resolved;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    if (resolve_target(device.addr, resolved)) {
        if (!target.name[0]) {
            strncpy(target.name, resolved.name, sizeof(target.name) - 1);
            target.name[sizeof(target.name) - 1] = 0;
        }
        if (!target.rssi) target.rssi = resolved.rssi;
        target.addr_type = resolved.addr_type;
    }
    strncpy(manual_target_, target.addr,
            sizeof(manual_target_) - 1);
    manual_target_[sizeof(manual_target_) - 1] = 0;
    manual_target_device_ = target;
    manual_connect_requested_ = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    ensure_task();
    Log::logf(CAT_OXI, LOG_INFO,
              "Sensor connect queued addr=%s type=%u name=\"%s\"\n",
              target.addr,
              static_cast<unsigned>(target.addr_type),
              target.name);
    return true;
}

bool BleSensorSource::request_disconnect(bool hold_until_absent) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    disconnect_requested_ = true;
    disconnect_hold_until_absent_ = hold_until_absent;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    return true;
}

bool BleSensorSource::forget(const char *addr_or_all) {
    if (!addr_or_all || !addr_or_all[0]) return false;
    bool changed = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    if (strcasecmp(addr_or_all, "all") == 0) {
        for (auto &dev : known_) dev = OximetrySensorDevice{};
        changed = true;
    } else {
        size_t index = 0;
        if (find_addr(addr_or_all, index)) {
            known_[index] = OximetrySensorDevice{};
            changed = true;
        }
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (changed) save_known();
    return changed;
}

bool BleSensorSource::set_autoconnect(const char *addr, bool enabled) {
    if (!addr || !addr[0]) return false;
    bool changed = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    size_t index = 0;
    if (find_addr(addr, index)) {
        known_[index].autoconnect = enabled;
        changed = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (changed) save_known();
    return changed;
}

void BleSensorSource::ensure_task() {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    if (task_started_) {
        portEXIT_CRITICAL(&mux_);
        return;
    }
    task_started_ = true;
    status_.task_started = true;
    portEXIT_CRITICAL(&mux_);
#if AC_STACK_PROFILE_ENABLED
    TaskHandle_t task = nullptr;
#endif
    const BaseType_t created =
        xTaskCreatePinnedToCore(task_entry,
                                "oxi_sensor",
                                AC_OXIMETRY_SENSOR_TASK_STACK,
                                this,
                                AC_OXIMETRY_SENSOR_TASK_PRIO,
#if AC_STACK_PROFILE_ENABLED
                                &task,
#else
                                nullptr,
#endif
                                0);
    portENTER_CRITICAL(&mux_);
    if (created == pdPASS) {
#if AC_STACK_PROFILE_ENABLED
        task_ = task;
#endif
    } else {
        task_started_ = false;
        status_.task_started = false;
#if AC_STACK_PROFILE_ENABLED
        task_ = nullptr;
#endif
    }
    portEXIT_CRITICAL(&mux_);
    if (created != pdPASS) {
        Log::logf(CAT_OXI, LOG_ERROR, "Sensor task create failed\n");
    }
#endif
}

void BleSensorSource::task_entry(void *param) {
    auto *self = static_cast<BleSensorSource *>(param);
    if (self) self->task_loop();
    vTaskDelete(nullptr);
}

void BleSensorSource::set_state(OximetrySensorState state) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    status_.state = state;
    status_.scanning = state == OximetrySensorState::Scanning;
    status_.connected =
        state == OximetrySensorState::Connected ||
        state == OximetrySensorState::Streaming;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

void BleSensorSource::set_error(const char *text) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    strncpy(status_.last_error, text ? text : "",
            sizeof(status_.last_error) - 1);
    status_.last_error[sizeof(status_.last_error) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

void BleSensorSource::hold_autoconnect(const char *addr, uint32_t now_ms,
                                       bool until_absent) {
    bool changed = true;
    char logged_addr[sizeof(auto_holdoff_addr_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    const char *hold_addr = addr ? addr : "";
    const bool same_addr =
        strcasecmp(auto_holdoff_addr_, hold_addr) == 0;
    const bool timed_hold_active =
        auto_holdoff_until_ms_ &&
        static_cast<int32_t>(now_ms - auto_holdoff_until_ms_) < 0;
    if (same_addr && auto_holdoff_until_absent_ == until_absent &&
        timed_hold_active) {
        changed = false;
    } else {
        auto_holdoff_until_absent_ = until_absent;
        auto_holdoff_until_ms_ =
            now_ms + AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS;
        strncpy(auto_holdoff_addr_, hold_addr,
                sizeof(auto_holdoff_addr_) - 1);
        auto_holdoff_addr_[sizeof(auto_holdoff_addr_) - 1] = 0;
    }
    strncpy(logged_addr, auto_holdoff_addr_, sizeof(logged_addr) - 1);
    logged_addr[sizeof(logged_addr) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (!changed) return;
    if (until_absent) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor auto-connect holdoff addr=%s until=absent max_ms=%lu\n",
                  logged_addr[0] ? logged_addr : "*",
                  static_cast<unsigned long>(
                      AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS));
    } else {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor auto-connect holdoff addr=%s ms=%lu\n",
                  logged_addr[0] ? logged_addr : "*",
                  static_cast<unsigned long>(
                      AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS));
    }
}

bool BleSensorSource::autoconnect_holdoff_active(uint32_t now_ms) const {
    bool active = false;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    if (auto_holdoff_addr_[0]) {
        active = auto_holdoff_until_ms_ &&
                 static_cast<int32_t>(
                     now_ms - auto_holdoff_until_ms_) < 0;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&mux_));
#endif
    return active;
}

void BleSensorSource::store_scan_result(const char *addr,
                                         uint8_t addr_type,
                                         const char *name,
                                         int rssi) {
    if (!addr || !addr[0]) return;
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    for (uint8_t i = 0; i < scan_count_; ++i) {
        if (strcasecmp(scan_results_[i].addr, addr) == 0) {
            scan_results_[i].rssi = rssi;
#if AC_OXIMETRY_BLE_ENABLED
            portEXIT_CRITICAL(&mux_);
#endif
            return;
        }
    }
    if (scan_count_ < AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS) {
        OximetrySensorDevice &dev = scan_results_[scan_count_++];
        strncpy(dev.addr, addr, sizeof(dev.addr) - 1);
        dev.addr[sizeof(dev.addr) - 1] = 0;
        dev.addr_type = addr_type;
        strncpy(dev.name, name ? name : "", sizeof(dev.name) - 1);
        dev.name[sizeof(dev.name) - 1] = 0;
        dev.rssi = rssi;
        dev.autoconnect = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

bool BleSensorSource::pick_autoconnect_target(OximetrySensorDevice &target,
                                               uint32_t now_ms) {
    int best_rssi = -999;
    bool found = false;
    bool holdoff_cleared = false;
    char holdoff_cleared_addr[sizeof(auto_holdoff_addr_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    bool holdoff_seen = false;
    bool timed_hold_active =
        auto_holdoff_until_ms_ &&
        static_cast<int32_t>(now_ms - auto_holdoff_until_ms_) < 0;
    if (!timed_hold_active) {
        auto_holdoff_addr_[0] = 0;
        auto_holdoff_until_ms_ = 0;
        auto_holdoff_until_absent_ = false;
    }
    for (uint8_t i = 0; i < scan_count_; ++i) {
        const bool matches_holdoff =
            auto_holdoff_addr_[0] &&
            strcasecmp(auto_holdoff_addr_,
                       scan_results_[i].addr) == 0;
        if (matches_holdoff) holdoff_seen = true;
        const bool holdoff_active =
            matches_holdoff && timed_hold_active;
        if (holdoff_active) continue;
        for (const auto &known : known_) {
            if (!known.addr[0] || !known.autoconnect) continue;
            if (strcasecmp(known.addr, scan_results_[i].addr) != 0) {
                continue;
            }
            if (!found || scan_results_[i].rssi > best_rssi) {
                target = scan_results_[i];
                target.autoconnect = known.autoconnect;
                best_rssi = target.rssi;
                found = true;
            }
        }
    }
    if (auto_holdoff_until_absent_ &&
        auto_holdoff_addr_[0] && !holdoff_seen) {
        strncpy(holdoff_cleared_addr, auto_holdoff_addr_,
                sizeof(holdoff_cleared_addr) - 1);
        holdoff_cleared_addr[sizeof(holdoff_cleared_addr) - 1] = 0;
        auto_holdoff_addr_[0] = 0;
        auto_holdoff_until_ms_ = 0;
        auto_holdoff_until_absent_ = false;
        holdoff_cleared = true;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (holdoff_cleared) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor auto-connect holdoff cleared addr=%s\n",
                  holdoff_cleared_addr);
    }
    return found;
}

void BleSensorSource::task_loop() {
#if AC_OXIMETRY_BLE_ENABLED
    set_state(OximetrySensorState::Idle);
    SensorBleScanCallbacks scan_callbacks(this);
    SensorBleClientCallbacks client_callbacks(this);
    uint32_t next_auto_scan_ms = 0;

    while (true) {
        bool enabled = false;
        char runtime_name[sizeof(runtime_name_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        enabled = enabled_;
        strncpy(runtime_name, runtime_name_, sizeof(runtime_name) - 1);
        runtime_name[sizeof(runtime_name) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        if (!enabled) {
            if (client_ && client_->isConnected()) {
                client_->disconnect();
            }
            set_state(OximetrySensorState::Off);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!runtime_.ensure_started(runtime_name)) {
            set_error("BLE init failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!client_) {
            client_ = NimBLEDevice::createClient();
            if (client_) {
                client_->setClientCallbacks(&client_callbacks, false);
                client_->setConnectionParams(
                    AC_OXIMETRY_SENSOR_CONN_INTERVAL_MIN,
                    AC_OXIMETRY_SENSOR_CONN_INTERVAL_MAX,
                    AC_OXIMETRY_SENSOR_CONN_LATENCY,
                    AC_OXIMETRY_SENSOR_CONN_TIMEOUT);
            }
        }
        if (!client_) {
            set_error("BLE sensor client failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        bool disconnect_now = false;
        bool disconnect_hold_until_absent = false;
        bool manual_scan = false;
        bool manual_connect = false;
        char manual_target[sizeof(manual_target_)] = {};
        OximetrySensorDevice manual_target_device;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        disconnect_now = disconnect_requested_;
        disconnect_hold_until_absent = disconnect_hold_until_absent_;
        manual_scan = scan_requested_;
        manual_connect = manual_connect_requested_;
        strncpy(manual_target, manual_target_,
                sizeof(manual_target) - 1);
        manual_target[sizeof(manual_target) - 1] = 0;
        manual_target_device = manual_target_device_;
        disconnect_requested_ = false;
        disconnect_hold_until_absent_ = false;
        scan_requested_ = false;
        manual_connect_requested_ = false;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif

        if (disconnect_now) {
            char holdoff_addr[sizeof(connected_addr_)] = {};
#if AC_OXIMETRY_BLE_ENABLED
            portENTER_CRITICAL(&mux_);
#endif
            strncpy(holdoff_addr, connected_addr_,
                    sizeof(holdoff_addr) - 1);
            holdoff_addr[sizeof(holdoff_addr) - 1] = 0;
#if AC_OXIMETRY_BLE_ENABLED
            portEXIT_CRITICAL(&mux_);
#endif
            if (client_->isConnected()) client_->disconnect();
            protocols_.reset();
            hold_autoconnect(holdoff_addr, millis(),
                             disconnect_hold_until_absent);
            set_state(OximetrySensorState::Idle);
        }

        const uint32_t now_ms = millis();
        protocols_.poll(now_ms);

        bool auto_allowed = false;
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        auto_allowed = auto_allowed_;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        const bool auto_scan =
            auto_allowed &&
            has_autoconnect() &&
            !client_->isConnected() &&
            static_cast<int32_t>(now_ms - next_auto_scan_ms) >= 0;

        if (!manual_scan && !manual_connect && !auto_scan) {
            if (!client_->isConnected()) {
                set_state(OximetrySensorState::Idle);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (manual_connect && manual_target_device.addr[0]) {
            Log::logf(CAT_OXI, LOG_INFO,
                      "Sensor manual connect starting addr=%s type=%u name=\"%s\"\n",
                      manual_target_device.addr,
                      static_cast<unsigned>(manual_target_device.addr_type),
                      manual_target_device.name);
            (void)connect_target(manual_target_device, true);
            continue;
        }

        if (client_->isConnected() && (manual_scan || manual_connect)) {
            client_->disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        NimBLEScan *scan = NimBLEDevice::getScan();
        if (!scan) {
            set_error("BLE scan unavailable");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        scan_count_ = 0;
        for (auto &dev : scan_results_) dev = OximetrySensorDevice{};
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        status_.scans++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        set_state(OximetrySensorState::Scanning);
        const log_level_t scan_log_level =
            (manual_scan || manual_connect) ? LOG_INFO : LOG_DEBUG;
        Log::logf(CAT_OXI, scan_log_level,
                  "Sensor scan started manual=%s connect=%s auto=%s\n",
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
        portENTER_CRITICAL(&mux_);
#endif
        scan_log_count = scan_count_;
        if (scan_log_count > AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS) {
            scan_log_count = AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS;
        }
        for (size_t i = 0; i < scan_log_count; ++i) {
            scan_log[i] = scan_results_[i];
        }
        scan_generation_++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        Log::logf(CAT_OXI, scan_log_level,
                  "Sensor scan complete count=%u\n",
                  static_cast<unsigned>(scan_log_count));
        for (size_t i = 0; i < scan_log_count; ++i) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor scan result %u addr=%s type=%u rssi=%d name=\"%s\"\n",
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
            portENTER_CRITICAL(&mux_);
#endif
            have_target = resolve_target(manual_target, target);
#if AC_OXIMETRY_BLE_ENABLED
            portEXIT_CRITICAL(&mux_);
#endif
        } else if (auto_scan && auto_allowed) {
            have_target = pick_autoconnect_target(target, now_ms);
        }

        if (have_target) {
            (void)connect_target(target, manual_connect);
        } else {
            if (manual_connect) {
                Log::logf(CAT_OXI, LOG_WARN,
                          "Sensor manual connect target not found target=%s count=%u\n",
                          manual_target[0] ? manual_target : "--",
                          static_cast<unsigned>(scan_log_count));
            } else if (auto_scan) {
                if (autoconnect_holdoff_active(millis())) {
                    Log::logf(CAT_OXI, LOG_DEBUG,
                              "Sensor auto-connect holdoff active\n");
                }
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "Sensor auto scan found no autoconnect target count=%u\n",
                          static_cast<unsigned>(scan_log_count));
            }
            set_state(OximetrySensorState::Idle);
        }
    }
#endif
}

bool BleSensorSource::connect_target(const OximetrySensorDevice &target,
                                     bool manual) {
#if AC_OXIMETRY_BLE_ENABLED
    if (!client_ || !target.addr[0]) return false;
    set_state(OximetrySensorState::Connecting);
    NimBLEDevice::getScan()->stop();
    if (client_->isConnected()) client_->disconnect();
    protocols_.reset();
    client_->cancelConnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    NimBLEAddress address(std::string(target.addr), target.addr_type);
    Log::logf(CAT_OXI, LOG_INFO,
              "Sensor connecting addr=%s type=%u name=\"%s\"\n",
              target.addr,
              static_cast<unsigned>(target.addr_type),
              target.name);
    if (!client_->connect(address)) {
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        status_.connect_failures++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor connect failed addr=%s err=%d\n",
                  target.addr,
                  client_->getLastError());
        set_state(OximetrySensorState::Idle);
        return false;
    }

    const bool needs_encryption = strncmp(target.name, "Nonin", 5) == 0;
    if (needs_encryption && !client_->secureConnection(false)) {
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        status_.connect_failures++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor encryption failed addr=%s err=%d\n",
                  target.addr,
                  client_->getLastError());
        client_->disconnect();
        set_state(OximetrySensorState::Idle);
        return false;
    }

    if (!subscribe_client(client_, target.name)) {
#if AC_OXIMETRY_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        status_.connect_failures++;
#if AC_OXIMETRY_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor has no supported oximetry service addr=%s\n",
                  target.addr);
        client_->disconnect();
        set_state(OximetrySensorState::Idle);
        return false;
    }

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    strncpy(connected_addr_, target.addr,
            sizeof(connected_addr_) - 1);
    connected_addr_[sizeof(connected_addr_) - 1] = 0;
    strncpy(connected_name_, target.name,
            sizeof(connected_name_) - 1);
    connected_name_[sizeof(connected_name_) - 1] = 0;
    if (manual) {
        size_t known_index = 0;
        if (!find_addr(target.addr, known_index)) {
            for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
                if (known_[i].addr[0]) continue;
                known_[i] = target;
                known_[i].autoconnect = true;
                break;
            }
        } else {
            known_[known_index].addr_type = target.addr_type;
            strncpy(known_[known_index].name, target.name,
                    sizeof(known_[known_index].name) - 1);
        }
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    if (manual) save_known();
    protocols_.on_connected();

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    status_.connects++;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    set_state(OximetrySensorState::Connected);
    Log::logf(CAT_OXI, LOG_INFO,
              "Sensor connected addr=%s name=\"%s\"\n",
              target.addr, target.name);
    return true;
#else
    (void)target;
    (void)manual;
    return false;
#endif
}

bool BleSensorSource::subscribe_client(void *client_ptr, const char *name) {
#if AC_OXIMETRY_BLE_ENABLED
    auto *client = static_cast<NimBLEClient *>(client_ptr);
    if (!client) return false;
    protocols_.reset();

    (void)name;
    return protocols_.subscribe(client);
#else
    (void)client_ptr;
    (void)name;
    return false;
#endif
}

bool BleSensorSource::take_event(BleSensorEvent &event) {
    event = BleSensorEvent{};
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    if (sample_pending_) {
        event.kind = BleSensorEventKind::Sample;
        event.sample = pending_sample_;
        sample_pending_ = false;
    } else if (disconnect_pending_) {
        event.kind = BleSensorEventKind::Disconnected;
        event.disconnect_reason = pending_disconnect_reason_;
        strncpy(event.disconnect_addr, pending_disconnect_addr_,
                sizeof(event.disconnect_addr) - 1);
        event.disconnect_addr[sizeof(event.disconnect_addr) - 1] = 0;
        disconnect_pending_ = false;
        pending_disconnect_addr_[0] = 0;
    }
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif

    if (event.kind == BleSensorEventKind::Disconnected) {
        Log::logf(CAT_OXI, LOG_INFO,
                  "Sensor disconnected addr=%s reason=%d\n",
                  event.disconnect_addr[0] ? event.disconnect_addr : "--",
                  event.disconnect_reason);
    }
    return event.kind != BleSensorEventKind::None;
}

void BleSensorSource::protocol_sample_callback(
    void *context,
    uint16_t spo2_raw,
    uint16_t pulse_raw,
    bool invalid,
    bool contact_known,
    bool contact_present) {
    auto *source = static_cast<BleSensorSource *>(context);
    if (!source) return;

    source->publish_sample(spo2_raw, pulse_raw, invalid,
                           contact_known, contact_present);
}

void BleSensorSource::publish_sample(uint16_t spo2_raw,
                                     uint16_t pulse_raw,
                                     bool from_invalid_packet,
                                     bool contact_known,
                                     bool contact_present) {
    bool spo2_valid = false;
    bool pulse_valid = false;
    const int16_t spo2 = decode_sfloat_int_value(spo2_raw, spo2_valid);
    const int16_t pulse = decode_sfloat_int_value(pulse_raw, pulse_valid);
    const bool valid = spo2_valid && pulse_valid && !from_invalid_packet;

#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    pending_sample_ = OximetrySample{};
    pending_sample_.source = OximetrySource::Ble;
    pending_sample_.spo2 = valid ? spo2 : -1;
    pending_sample_.pulse_bpm = valid ? pulse : -1;
    pending_sample_.valid = valid;
    pending_sample_.contact_known = contact_known;
    pending_sample_.contact_present = contact_present;
    if (connected_name_[0]) {
        snprintf(pending_sample_.detail, sizeof(pending_sample_.detail),
                 "%s %s", connected_addr_, connected_name_);
    } else {
        strncpy(pending_sample_.detail, connected_addr_,
                sizeof(pending_sample_.detail) - 1);
    }
    sample_pending_ = true;
    status_.notifications++;
    if (!valid) status_.invalid_notifications++;
    status_.state = OximetrySensorState::Streaming;
    status_.connected = true;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

void BleSensorSource::callback_disconnected(int reason) {
#if AC_OXIMETRY_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    disconnect_pending_ = true;
    pending_disconnect_reason_ = reason;
    strncpy(pending_disconnect_addr_, connected_addr_,
            sizeof(pending_disconnect_addr_) - 1);
    pending_disconnect_addr_[sizeof(pending_disconnect_addr_) - 1] = 0;
    connected_addr_[0] = 0;
    connected_name_[0] = 0;
    status_.connected = false;
    status_.state = OximetrySensorState::Idle;
    status_.disconnects++;
#if AC_OXIMETRY_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

}  // namespace aircannect
