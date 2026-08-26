#include "ble_runtime.h"

#include "board.h"

#if AC_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

#if AC_BLE_ENABLED
class BleRuntimeScanCallbacks : public NimBLEScanCallbacks {
public:
    explicit BleRuntimeScanCallbacks(BleRuntime *runtime)
        : runtime_(runtime) {}

    void onResult(const NimBLEAdvertisedDevice *device) override {
        if (runtime_ && device) runtime_->note_advertisement(device);
    }

    void onScanEnd(const NimBLEScanResults &results, int reason) override {
        (void)results;
        (void)reason;
        if (runtime_) runtime_->note_observer_stopped();
    }

private:
    BleRuntime *runtime_ = nullptr;
};

BleRuntime::ScanLease::ScanLease(ScanLease &&other) noexcept
    : runtime_(other.runtime_) {
    other.runtime_ = nullptr;
}

BleRuntime::ScanLease &BleRuntime::ScanLease::operator=(
    ScanLease &&other) noexcept {
    if (this == &other) return *this;

    release();
    runtime_ = other.runtime_;
    other.runtime_ = nullptr;
    return *this;
}

BleRuntime::ScanLease::~ScanLease() { release(); }

void BleRuntime::ScanLease::release() {
    if (!runtime_) return;
    runtime_->release_scan();
    runtime_ = nullptr;
}

bool BleRuntime::begin() {
    if (!init_mutex_) {
        init_mutex_ = xSemaphoreCreateMutexStatic(&init_mutex_storage_);
    }
    if (!scan_mutex_) {
        scan_mutex_ = xSemaphoreCreateMutexStatic(&scan_mutex_storage_);
    }
    if (!observer_callbacks_) {
        observer_callbacks_ = new BleRuntimeScanCallbacks(this);
    }
    return init_mutex_ != nullptr && scan_mutex_ != nullptr &&
           observer_callbacks_ != nullptr;
}

bool BleRuntime::ensure_started(const char *name) {
    if (!begin()) return false;
    if (xSemaphoreTake(init_mutex_, portMAX_DELAY) != pdTRUE) return false;

    if (!name || !name[0]) name = "aircannect";
    bool ready = true;
    if (!NimBLEDevice::isInitialized()) {
#if defined(CONFIG_BTDM_BLE_SCAN_DUPL) || defined(CONFIG_BT_LE_SCAN_DUPL) || \
    defined(CONFIG_BT_CTRL_BLE_SCAN_DUPL)
        NimBLEDevice::setScanDuplicateCacheSize(
            AC_BLE_SCAN_DUP_CACHE);
#endif
        ready = NimBLEDevice::init(name);
        if (ready) {
            NimBLEDevice::setSecurityAuth(true, false, false);
            NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        }
    }
    xSemaphoreGive(init_mutex_);
    return ready;
}

BleRuntime::ScanLease BleRuntime::acquire_scan(TickType_t timeout_ticks) {
    if (!begin() ||
        xSemaphoreTake(scan_mutex_, timeout_ticks) != pdTRUE) {
        return {};
    }
    if (!stop_passive_observer_locked()) {
        xSemaphoreGive(scan_mutex_);
        return {};
    }
    return ScanLease(this);
}

bool BleRuntime::scan_in_progress() const {
    return scan_mutex_ && uxSemaphoreGetCount(scan_mutex_) == 0;
}

void BleRuntime::release_scan() {
    if (!scan_mutex_) return;
    (void)start_passive_observer_locked();
    xSemaphoreGive(scan_mutex_);
}

void BleRuntime::set_passive_observer(BleAdvertisementHandler handler,
                                      void *context) {
    portENTER_CRITICAL(&observer_mux_);
    observer_handler_ = handler;
    observer_context_ = context;
    portEXIT_CRITICAL(&observer_mux_);
}

void BleRuntime::request_passive_observation(bool enabled) {
    portENTER_CRITICAL(&observer_mux_);
    observer_requested_ = enabled;
    portEXIT_CRITICAL(&observer_mux_);

    if (!scan_mutex_ ||
        xSemaphoreTake(scan_mutex_, 0) != pdTRUE) {
        return;
    }
    if (enabled) {
        (void)start_passive_observer_locked();
    } else {
        (void)stop_passive_observer_locked();
    }
    xSemaphoreGive(scan_mutex_);
}

bool BleRuntime::passive_observation_active() const {
    portENTER_CRITICAL(&observer_mux_);
    const bool active = observer_running_;
    portEXIT_CRITICAL(&observer_mux_);
    return active;
}

bool BleRuntime::start_passive_observer_locked() {
    BleAdvertisementHandler handler = nullptr;
    bool requested = false;
    uint32_t retry_ms = 0;
    portENTER_CRITICAL(&observer_mux_);
    handler = observer_handler_;
    requested = observer_requested_;
    retry_ms = observer_retry_ms_;
    const bool running = observer_running_;
    portEXIT_CRITICAL(&observer_mux_);

    const uint32_t now_ms = millis();
    if (running || !requested || !handler ||
        (retry_ms && static_cast<int32_t>(now_ms - retry_ms) < 0) ||
        !NimBLEDevice::isInitialized()) {
        return running || !requested;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan || scan->isScanning()) return false;

    scan->clearResults();
    scan->setScanCallbacks(observer_callbacks_, true);
    scan->setMaxResults(0);
    scan->setActiveScan(false);
    scan->setInterval(AC_BLE_OBSERVER_SCAN_INTERVAL_MS);
    scan->setWindow(AC_BLE_OBSERVER_SCAN_WINDOW_MS);
    const bool started = scan->start(0, false, true);

    portENTER_CRITICAL(&observer_mux_);
    observer_running_ = started;
    observer_retry_ms_ = started ? 0 : now_ms + AC_BLE_OBSERVER_RETRY_MS;
    portEXIT_CRITICAL(&observer_mux_);
    return started;
}

bool BleRuntime::stop_passive_observer_locked() {
    portENTER_CRITICAL(&observer_mux_);
    const bool running = observer_running_;
    observer_running_ = false;
    portEXIT_CRITICAL(&observer_mux_);
    if (!running) return true;

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan) return false;
    const bool stopped = scan->stop();
    scan->setScanCallbacks(nullptr, false);
    scan->clearResults();
    return stopped;
}

void BleRuntime::note_advertisement(const void *device_ptr) {
    const auto *device = static_cast<const NimBLEAdvertisedDevice *>(
        device_ptr);
    if (!device) return;

    BleAdvertisementHandler handler = nullptr;
    void *context = nullptr;
    portENTER_CRITICAL(&observer_mux_);
    handler = observer_handler_;
    context = observer_context_;
    const bool running = observer_running_;
    portEXIT_CRITICAL(&observer_mux_);
    if (!running || !handler) return;

    const NimBLEAddress &address = device->getAddress();
    const uint8_t *value = address.getVal();
    BleAdvertisement advertisement;
    snprintf(advertisement.address, sizeof(advertisement.address),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             value[5], value[4], value[3], value[2], value[1], value[0]);
    advertisement.address_type = address.getType();
    advertisement.rssi = device->getRSSI();
    handler(context, advertisement);
}

void BleRuntime::note_observer_stopped() {
    portENTER_CRITICAL(&observer_mux_);
    observer_running_ = false;
    portEXIT_CRITICAL(&observer_mux_);
}
#else
BleRuntime::ScanLease::ScanLease(ScanLease &&other) noexcept
    : runtime_(other.runtime_) {
    other.runtime_ = nullptr;
}

BleRuntime::ScanLease &BleRuntime::ScanLease::operator=(
    ScanLease &&other) noexcept {
    runtime_ = other.runtime_;
    other.runtime_ = nullptr;
    return *this;
}

BleRuntime::ScanLease::~ScanLease() = default;

void BleRuntime::ScanLease::release() { runtime_ = nullptr; }

bool BleRuntime::begin() { return true; }

bool BleRuntime::ensure_started(const char *name) {
    (void)name;
    return false;
}

BleRuntime::ScanLease BleRuntime::acquire_scan(TickType_t timeout_ticks) {
    (void)timeout_ticks;
    return {};
}

bool BleRuntime::scan_in_progress() const { return false; }

void BleRuntime::release_scan() {}

void BleRuntime::set_passive_observer(BleAdvertisementHandler handler,
                                      void *context) {
    (void)handler;
    (void)context;
}

void BleRuntime::request_passive_observation(bool enabled) {
    (void)enabled;
}

bool BleRuntime::passive_observation_active() const { return false; }

bool BleRuntime::start_passive_observer_locked() { return false; }

bool BleRuntime::stop_passive_observer_locked() { return true; }

void BleRuntime::note_advertisement(const void *device) { (void)device; }

void BleRuntime::note_observer_stopped() {}
#endif

}  // namespace aircannect
