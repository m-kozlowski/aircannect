#pragma once

#include "oximetry_internal.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
bool sensor_matches_supported_device(const NimBLEAdvertisedDevice *dev);
bool sensor_subscribe_supported_device(NimBLEClient *client);
bool sensor_subscribe_plx(NimBLEClient *client);
bool sensor_subscribe_nonin(NimBLEClient *client);
bool sensor_subscribe_viatom(NimBLEClient *client);
void sensor_viatom_on_connected();
void sensor_viatom_reset();
void sensor_viatom_poll(uint32_t now_ms);
#endif

}  // namespace aircannect
