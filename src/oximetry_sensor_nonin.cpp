#include "ble_sensor_protocols.h"

#include "debug_log.h"
#include "oximetry_codec.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
bool BleSensorProtocolEngine::subscribe_nonin() {
    if (!client_) return false;

    NimBLERemoteService *service = client_->getService(NimBLEUUID(
        "46A970E0-0D5F-11E2-8B5E-0002A5D5C51B"));
    if (!service) return false;

    NimBLERemoteCharacteristic *continuous =
        service->getCharacteristic(NimBLEUUID(
            "0AAD7EA0-0D60-11E2-8E3C-0002A5D5C51B"));
    if (!continuous || !continuous->canNotify() ||
        !continuous->subscribe(true, nonin_notify)) {
        return false;
    }

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor subscribed Nonin continuous\n");
    return true;
}

void BleSensorProtocolEngine::nonin_notify(
    NimBLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t len,
    bool is_notify) {
    (void)characteristic;
    (void)is_notify;
    BleSensorProtocolEngine *engine =
        active_.load(std::memory_order_acquire);
    if (!engine || !data || len < 5) return;

    const uint8_t spo2 = data[2];
    const uint16_t pulse =
        static_cast<uint16_t>(data[3]) |
        (static_cast<uint16_t>(data[4]) << 8);
    const bool valid = spo2 > 0 && spo2 <= 100 &&
                       pulse > 0 && pulse < 500;

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Nonin reading %s spo2=%u pulse=%u\n",
              valid ? "valid" : "invalid",
              static_cast<unsigned>(spo2),
              static_cast<unsigned>(pulse));
    engine->emit_sample(
        valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN,
        valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN,
        !valid);
}
#endif

}  // namespace aircannect
