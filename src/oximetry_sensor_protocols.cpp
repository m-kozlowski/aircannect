#include "oximetry_sensor_protocols.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
bool sensor_matches_supported_device(const NimBLEAdvertisedDevice *dev) {
    if (!dev) return false;

    const std::string name = dev->getName();
    return dev->isAdvertisingService(NimBLEUUID(PLX_SERVICE_UUID)) ||
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
}

bool sensor_subscribe_supported_device(NimBLEClient *client) {
    return sensor_subscribe_plx(client) ||
           sensor_subscribe_nonin(client) ||
           sensor_subscribe_viatom(client);
}
#endif

}  // namespace aircannect
