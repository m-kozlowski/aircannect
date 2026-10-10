#pragma once

#include <esp_err.h>

namespace aircannect::StorageUsbDevice {

// Restore the default PHY before Serial.begin(), including after OTA from MSC.
void begin();

// Called only by the storage task, after local SD access has stopped.
esp_err_t start();
esp_err_t stop();

// Serial/JTAG stays allocated but has no PHY while MSC owns the USB port.
bool serial_available();

}  // namespace aircannect::StorageUsbDevice
