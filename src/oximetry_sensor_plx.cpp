#include "ble_sensor_protocols.h"

#include "debug_log.h"
#include "oximetry_codec.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
bool BleSensorProtocolEngine::subscribe_plx() {
    if (!client_) return false;

    NimBLERemoteService *service =
        client_->getService(NimBLEUUID("1822"));
    if (!service) return false;

    NimBLERemoteCharacteristic *continuous =
        service->getCharacteristic(NimBLEUUID("2A5F"));
    if (continuous && continuous->canNotify() &&
        continuous->subscribe(true, plx_notify)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor subscribed PLX continuous\n");
        return true;
    }

    NimBLERemoteCharacteristic *spot =
        service->getCharacteristic(NimBLEUUID("2A5E"));
    if (spot && (spot->canNotify() || spot->canIndicate()) &&
        spot->subscribe(spot->canNotify(), plx_notify)) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Sensor subscribed PLX spot\n");
        return true;
    }

    return false;
}

void BleSensorProtocolEngine::plx_notify(
    NimBLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t len,
    bool is_notify) {
    (void)characteristic;
    (void)is_notify;
    BleSensorProtocolEngine *engine =
        active_.load(std::memory_order_acquire);
    if (!engine || !data || len < 5) return;

    const uint16_t spo2_raw =
        static_cast<uint16_t>(data[1]) |
        (static_cast<uint16_t>(data[2]) << 8);
    const uint16_t pulse_raw =
        static_cast<uint16_t>(data[3]) |
        (static_cast<uint16_t>(data[4]) << 8);
    bool spo2_valid = false;
    bool pulse_valid = false;
    const int16_t spo2 = decode_sfloat_int_value(spo2_raw, spo2_valid);
    const int16_t pulse = decode_sfloat_int_value(pulse_raw, pulse_valid);
    const bool valid = spo2_valid && pulse_valid;

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor PLX reading %s spo2=%d pulse=%d\n",
              valid ? "valid" : "invalid",
              static_cast<int>(spo2), static_cast<int>(pulse));
    engine->emit_sample(spo2_raw, pulse_raw, !valid);
}
#endif

}  // namespace aircannect
