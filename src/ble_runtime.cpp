#include "ble_runtime.h"

#include "board.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
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
    return init_mutex_ != nullptr && scan_mutex_ != nullptr;
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
            AC_OXIMETRY_BLE_SCAN_DUP_CACHE);
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
    return ScanLease(this);
}

bool BleRuntime::scan_in_progress() const {
    return scan_mutex_ && uxSemaphoreGetCount(scan_mutex_) == 0;
}

void BleRuntime::release_scan() {
    if (scan_mutex_) xSemaphoreGive(scan_mutex_);
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
#endif

}  // namespace aircannect
