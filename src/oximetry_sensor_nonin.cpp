#include "oximetry_sensor_protocols.h"

#include "debug_log.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
namespace {

void sensor_nonin_notify_cb(NimBLERemoteCharacteristic *chr,
                            uint8_t *data,
                            size_t len,
                            bool is_notify) {
    (void)chr;
    (void)is_notify;
    if (len < 5 || !data) return;
    const uint8_t spo2 = data[2];
    const uint16_t pulse =
        static_cast<uint16_t>(data[3]) |
        (static_cast<uint16_t>(data[4]) << 8);
    const bool valid = spo2 > 0 && spo2 <= 100 && pulse > 0 && pulse < 500;
    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Nonin reading %s spo2=%u pulse=%u\n",
              valid ? "valid" : "invalid",
              static_cast<unsigned>(spo2),
              static_cast<unsigned>(pulse));
    if (sensor_owner) {
        sensor_owner->on_sensor_sample(
            valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN,
            valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN,
            !valid);
    }
}

}  // namespace

bool sensor_subscribe_nonin(NimBLEClient *client) {
    if (!client) return false;
    NimBLERemoteService *nonin_service =
        client->getService(NimBLEUUID(NONIN_SERVICE_UUID));
    if (!nonin_service) return false;

    NimBLERemoteCharacteristic *continuous =
        nonin_service->getCharacteristic(NimBLEUUID(NONIN_CONTINUOUS_UUID));
    if (continuous && continuous->canNotify() &&
        continuous->subscribe(true, sensor_nonin_notify_cb)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor subscribed Nonin continuous\n");
        return true;
    }

    return false;
}
#endif

}  // namespace aircannect
