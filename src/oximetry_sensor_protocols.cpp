#include "ble_sensor_protocols.h"

#include "oximetry_codec.h"

namespace aircannect {

void BleSensorProtocolEngine::set_sample_callback(SampleCallback callback,
                                                   void *context) {
    sample_callback_ = callback;
    sample_context_ = context;
}

void BleSensorProtocolEngine::emit_sample(uint16_t spo2_raw,
                                          uint16_t pulse_raw,
                                          bool invalid,
                                          bool contact_known,
                                          bool contact_present) {
    if (!sample_callback_) return;

    sample_callback_(sample_context_, spo2_raw, pulse_raw, invalid,
                     contact_known, contact_present);
}

#if AC_OXIMETRY_BLE_ENABLED
std::atomic<BleSensorProtocolEngine *> BleSensorProtocolEngine::active_ =
    nullptr;

bool BleSensorProtocolEngine::matches(
    const NimBLEAdvertisedDevice *device) const {
    if (!device) return false;

    const std::string name = device->getName();
    bool oxyii_manufacturer = false;
    for (uint8_t i = 0; i < device->getManufacturerDataCount(); ++i) {
        const std::string data = device->getManufacturerData(i);
        if (data.size() < 2) continue;

        const uint16_t company =
            static_cast<uint8_t>(data[0]) |
            (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
        if (company == 0x036f || company == 0xf34e) {
            oxyii_manufacturer = true;
            break;
        }
    }

    return oxyii_manufacturer ||
           device->isAdvertisingService(NimBLEUUID("1822")) ||
           device->isAdvertisingService(NimBLEUUID(
               "46A970E0-0D5F-11E2-8B5E-0002A5D5C51B")) ||
           device->isAdvertisingService(NimBLEUUID(
               "14839AC4-7D7E-415C-9A42-167340CF2339")) ||
           device->isAdvertisingService(NimBLEUUID(
               "E8FB0001-A14B-98F9-831B-4E2941D01248")) ||
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

bool BleSensorProtocolEngine::subscribe(NimBLEClient *client) {
    reset();
    if (!client) return false;

    client_ = client;
    active_.store(this, std::memory_order_release);
    if (subscribe_plx()) {
        active_protocol_ = ActiveProtocol::Plx;
        return true;
    }
    if (subscribe_nonin()) {
        active_protocol_ = ActiveProtocol::Nonin;
        return true;
    }
    if (subscribe_viatom()) {
        active_protocol_ = ActiveProtocol::Viatom;
        return true;
    }
    if (subscribe_oxyii()) {
        active_protocol_ = ActiveProtocol::Oxyii;
        return true;
    }

    reset();
    return false;
}
#endif

void BleSensorProtocolEngine::on_connected() {
#if AC_OXIMETRY_BLE_ENABLED
    switch (active_protocol_) {
        case ActiveProtocol::Viatom:
            viatom_on_connected();
            break;
        case ActiveProtocol::Oxyii:
            oxyii_on_connected();
            break;
        case ActiveProtocol::None:
        case ActiveProtocol::Plx:
        case ActiveProtocol::Nonin:
            break;
    }
#endif
}

void BleSensorProtocolEngine::reset() {
#if AC_OXIMETRY_BLE_ENABLED
    viatom_reset();
    oxyii_reset();
    client_ = nullptr;
    BleSensorProtocolEngine *expected = this;
    (void)active_.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel);
#endif
    active_protocol_ = ActiveProtocol::None;
}

void BleSensorProtocolEngine::poll(uint32_t now_ms) {
#if AC_OXIMETRY_BLE_ENABLED
    if (!client_ || !client_->isConnected()) return;

    switch (active_protocol_) {
        case ActiveProtocol::Viatom:
            viatom_poll(now_ms);
            break;
        case ActiveProtocol::Oxyii:
            oxyii_poll(now_ms);
            break;
        case ActiveProtocol::None:
        case ActiveProtocol::Plx:
        case ActiveProtocol::Nonin:
            break;
    }
#else
    (void)now_ms;
#endif
}

}  // namespace aircannect
