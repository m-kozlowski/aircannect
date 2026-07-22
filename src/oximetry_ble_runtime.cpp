#include "oximetry_ble_runtime.h"

#include "board.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
bool OximetryBleRuntime::begin() {
    if (mutex_) return true;
    mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    return mutex_ != nullptr;
}

bool OximetryBleRuntime::ensure_started(const char *name) {
    if (!begin()) return false;
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) return false;

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
    xSemaphoreGive(mutex_);
    return ready;
}
#else
bool OximetryBleRuntime::begin() { return true; }

bool OximetryBleRuntime::ensure_started(const char *name) {
    (void)name;
    return false;
}
#endif

}  // namespace aircannect
