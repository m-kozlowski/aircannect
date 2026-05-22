#include "oximetry_sensor_protocols.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
bool sensor_matches_supported_device(const NimBLEAdvertisedDevice *dev) {
    if (!dev) return false;

    const std::string name = dev->getName();
    bool oxyii_mfg = false;
    for (uint8_t i = 0; i < dev->getManufacturerDataCount(); ++i) {
        const std::string data = dev->getManufacturerData(i);
        if (data.size() < 2) continue;
        const uint16_t company =
            static_cast<uint8_t>(data[0]) |
            (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
        if (company == 0x036f || company == 0xf34e) {
            oxyii_mfg = true;
            break;
        }
    }

    return oxyii_mfg ||
           dev->isAdvertisingService(NimBLEUUID(PLX_SERVICE_UUID)) ||
           dev->isAdvertisingService(NimBLEUUID(NONIN_SERVICE_UUID)) ||
           dev->isAdvertisingService(NimBLEUUID(VIATOM_SERVICE_UUID)) ||
           dev->isAdvertisingService(NimBLEUUID(OXYII_SERVICE_UUID)) ||
           name.rfind("Nonin", 0) == 0 ||
           name.rfind("O2 ", 0) == 0 ||
           name.rfind("O2Ring", 0) == 0 ||
           name.rfind("O2M", 0) == 0 ||
           name.rfind("S8-AW", 0) == 0 ||
           name.rfind("T8520_", 0) == 0 ||
           name.rfind("CheckMe", 0) == 0 ||
           name.rfind("Checkme", 0) == 0 ||
           name.rfind("CheckO2", 0) == 0 ||
           name.rfind("SleepU", 0) == 0 ||
           name.rfind("SleepO2", 0) == 0 ||
           name.rfind("WearO2", 0) == 0 ||
           name.rfind("KidsO2", 0) == 0 ||
           name.rfind("BabyO2", 0) == 0 ||
           name.rfind("OxyLink", 0) == 0 ||
           name.rfind("Oxylink", 0) == 0;
}

bool sensor_subscribe_supported_device(NimBLEClient *client) {
    return sensor_subscribe_plx(client) ||
           sensor_subscribe_nonin(client) ||
           sensor_subscribe_viatom(client) ||
           sensor_subscribe_oxyii(client);
}

void sensor_protocols_on_connected() {
    sensor_viatom_on_connected();
    sensor_oxyii_on_connected();
}

void sensor_protocols_reset() {
    sensor_viatom_reset();
    sensor_oxyii_reset();
}

void sensor_protocols_poll(uint32_t now_ms) {
    sensor_viatom_poll(now_ms);
    sensor_oxyii_poll(now_ms);
}
#endif

}  // namespace aircannect
