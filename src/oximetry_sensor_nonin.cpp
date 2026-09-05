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

    notify_characteristic_.store(continuous, std::memory_order_release);
    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor subscribed Nonin continuous\n");
    return true;
}

void BleSensorProtocolEngine::nonin_notify(
    NimBLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t len,
    bool is_notify) {
    (void)is_notify;
    BleSensorProtocolEngine *engine =
        active_.load(std::memory_order_acquire);
    if (engine) {
        (void)engine->enqueue_notification(
            ActiveProtocol::Nonin, characteristic, data, len);
    }
}

void BleSensorProtocolEngine::process_nonin_notification(
    const uint8_t *data,
    size_t len) {
    NoninDf19Reading reading;
    if (!decode_nonin_df19(data, len, reading)) return;

    // Keep DF19 status handling aligned with the existing source policy.
    const bool valid = reading.valid;

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Nonin reading %s spo2=%u pulse=%u\n",
              valid ? "valid" : "invalid",
              static_cast<unsigned>(reading.spo2),
              static_cast<unsigned>(reading.pulse_bpm));
    emit_sample(
        valid ? encode_sfloat_int_value(reading.spo2) : PLX_SFLOAT_NAN,
        valid ? encode_sfloat_int_value(reading.pulse_bpm) : PLX_SFLOAT_NAN,
        !valid);
}
#endif

}  // namespace aircannect
