#include "ble_runtime.h"

#include <cstring>
#include <exception>

#include "board.h"
#include "debug_log.h"

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

namespace {
bool configure_observer_filter(const BleObserverTarget *targets, size_t count) {
    if (!count) return false;

    ble_addr_t addresses[AC_BLE_OBSERVER_MAX_TARGETS * 2] = {};
    for (size_t i = 0; i < count; ++i) {
        if (targets[i].address_type > BLE_ADDR_RANDOM) return false;

        NimBLEAddress address;
        try {
            address = NimBLEAddress(targets[i].address,
                                    targets[i].address_type);
        } catch (const std::exception &) {
            return false;
        }
        if (address.isNull() ||
            (!address.isPublic() && !address.isStatic())) {
            return false;
        }

        // Sensor matching is address-only, including legacy saved types.
        addresses[i * 2] = *address.getBase();
        addresses[i * 2].type = BLE_ADDR_PUBLIC;
        addresses[i * 2 + 1] = *address.getBase();
        addresses[i * 2 + 1].type = BLE_ADDR_RANDOM;
    }

    // Install the complete list at once; a partial list must never filter.
    const int rc = ble_gap_wl_set(addresses, count * 2);
    if (rc != 0) {
        Log::logf(CAT_BLE, LOG_WARN,
                  "observer filter unavailable rc=%d; scanning all peers\n",
                  rc);
    }
    return rc == 0;
}
}  // namespace

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

    portENTER_CRITICAL(&observer_mux_);
    const bool requested = observer_requested_ && observer_allowed_;
    portEXIT_CRITICAL(&observer_mux_);

    if (requested) {
        (void)start_passive_observer_locked();
    } else {
        (void)stop_passive_observer_locked();
    }

    portENTER_CRITICAL(&observer_mux_);
    const bool settled = (observer_requested_ && observer_allowed_) == requested &&
        (requested
             ? observer_running_ &&
                   observer_applied_revision_ == observer_targets_revision_
             : !observer_running_);
    observer_reconcile_pending_ = !settled;
    portEXIT_CRITICAL(&observer_mux_);

    xSemaphoreGive(scan_mutex_);
}

void BleRuntime::set_passive_observer(BleAdvertisementHandler handler,
                                      void *context) {
    portENTER_CRITICAL(&observer_mux_);
    observer_handler_ = handler;
    observer_context_ = context;
    portEXIT_CRITICAL(&observer_mux_);
}

void BleRuntime::set_passive_observer_targets(
    const BleObserverTarget *targets, size_t count) {
    if (!targets || count > AC_BLE_OBSERVER_MAX_TARGETS) count = 0;

    portENTER_CRITICAL(&observer_mux_);
    if (observer_target_count_ != count ||
        (count && memcmp(observer_targets_, targets,
                         count * sizeof(*targets)) != 0)) {
        if (count) memcpy(observer_targets_, targets, count * sizeof(*targets));
        observer_target_count_ = count;
        ++observer_targets_revision_;
    }
    portEXIT_CRITICAL(&observer_mux_);
}

bool BleRuntime::request_passive_observation(bool enabled) {
    portENTER_CRITICAL(&observer_mux_);
    observer_requested_ = enabled;
    portEXIT_CRITICAL(&observer_mux_);
    return reconcile_passive_observation();
}

void BleRuntime::set_passive_observation_allowed(bool allowed) {
    portENTER_CRITICAL(&observer_mux_);
    observer_allowed_ = allowed;
    portEXIT_CRITICAL(&observer_mux_);
    (void)reconcile_passive_observation();
}

bool BleRuntime::reconcile_passive_observation() {
    portENTER_CRITICAL(&observer_mux_);
    const bool enabled = observer_requested_ && observer_allowed_;
    const bool settled = !observer_reconcile_pending_ &&
        (enabled
             ? observer_running_ &&
                   observer_applied_revision_ == observer_targets_revision_
             : !observer_running_);
    portEXIT_CRITICAL(&observer_mux_);
    if (settled) return true;

    if (!scan_mutex_ ||
        xSemaphoreTake(scan_mutex_, 0) != pdTRUE) {
        portENTER_CRITICAL(&observer_mux_);
        observer_reconcile_pending_ = true;
        portEXIT_CRITICAL(&observer_mux_);
        return false;
    }
    bool applied = false;
    if (enabled) {
        applied = start_passive_observer_locked();
    } else {
        applied = stop_passive_observer_locked();
    }

    portENTER_CRITICAL(&observer_mux_);
    const bool still_requested =
        (observer_requested_ && observer_allowed_) == enabled;
    const bool state_matches = enabled
        ? observer_running_ &&
              observer_applied_revision_ == observer_targets_revision_
        : !observer_running_;
    observer_reconcile_pending_ = !applied || !still_requested || !state_matches;
    const bool reconciled = !observer_reconcile_pending_;
    portEXIT_CRITICAL(&observer_mux_);

    xSemaphoreGive(scan_mutex_);
    return reconciled;
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
    BleObserverTarget targets[AC_BLE_OBSERVER_MAX_TARGETS] = {};
    size_t target_count = 0;
    uint32_t targets_revision = 0;
    uint32_t applied_revision = 0;

    portENTER_CRITICAL(&observer_mux_);
    handler = observer_handler_;
    requested = observer_requested_ && observer_allowed_;
    retry_ms = observer_retry_ms_;
    const bool running = observer_running_;
    target_count = observer_target_count_;
    targets_revision = observer_targets_revision_;
    applied_revision = observer_applied_revision_;
    memcpy(targets, observer_targets_, target_count * sizeof(*targets));
    portEXIT_CRITICAL(&observer_mux_);

    const uint32_t now_ms = millis();
    if ((running && targets_revision == applied_revision) ||
        !requested || !handler ||
        (retry_ms && static_cast<int32_t>(now_ms - retry_ms) < 0) ||
        !NimBLEDevice::isInitialized()) {
        return running || !requested;
    }

    if (running && !stop_passive_observer_locked()) return false;

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan || scan->isScanning()) return false;

    scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
    const bool filtered = configure_observer_filter(targets, target_count);
    if (filtered) {
        scan->setFilterPolicy(BLE_HCI_SCAN_FILT_USE_WL);
    }

    scan->clearResults();
    scan->setScanCallbacks(observer_callbacks_, true);
    scan->setMaxResults(0);
    scan->setActiveScan(false);
    scan->setInterval(AC_BLE_OBSERVER_SCAN_INTERVAL_MS);
    scan->setWindow(AC_BLE_OBSERVER_SCAN_WINDOW_MS);
    bool started = scan->start(0, false, true);
    if (!started) {
        scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
        if (filtered) started = scan->start(0, false, true);
    }
    portENTER_CRITICAL(&observer_mux_);
    observer_applied_revision_ = targets_revision;
    observer_running_ = started;
    observer_retry_ms_ = started ? 0 : now_ms + AC_BLE_OBSERVER_RETRY_MS;
    portEXIT_CRITICAL(&observer_mux_);
    return started;
}

bool BleRuntime::stop_passive_observer_locked() {
    portENTER_CRITICAL(&observer_mux_);
    const bool running = observer_running_;
    portEXIT_CRITICAL(&observer_mux_);
    if (!NimBLEDevice::isInitialized()) return !running;

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan) return false;
    if (running && !scan->stop()) return false;

    scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
    if (!running) return true;

    portENTER_CRITICAL(&observer_mux_);
    observer_running_ = false;
    portEXIT_CRITICAL(&observer_mux_);
    scan->setScanCallbacks(nullptr, false);
    scan->clearResults();
    return true;
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

void BleRuntime::set_passive_observer_targets(
    const BleObserverTarget *targets, size_t count) {
    (void)targets;
    (void)count;
}

bool BleRuntime::request_passive_observation(bool enabled) {
    (void)enabled;
    return !enabled;
}

void BleRuntime::set_passive_observation_allowed(bool allowed) {
    (void)allowed;
}

bool BleRuntime::reconcile_passive_observation() { return true; }

bool BleRuntime::passive_observation_active() const { return false; }

bool BleRuntime::start_passive_observer_locked() { return false; }

bool BleRuntime::stop_passive_observer_locked() { return true; }

void BleRuntime::note_advertisement(const void *device) { (void)device; }

void BleRuntime::note_observer_stopped() {}
#endif

}  // namespace aircannect
