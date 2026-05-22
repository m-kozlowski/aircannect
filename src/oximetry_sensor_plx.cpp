#include "oximetry_sensor_protocols.h"

#include "debug_log.h"

namespace aircannect {

#if AC_OXIMETRY_BLE_ENABLED
namespace {

void sensor_plx_notify_cb(NimBLERemoteCharacteristic *chr,
                          uint8_t *data,
                          size_t len,
                          bool is_notify) {
    (void)chr;
    (void)is_notify;
    if (len < 5 || !data) return;
    if (sensor_owner) {
        sensor_owner->on_sensor_sample(
            static_cast<uint16_t>(data[1]) |
                (static_cast<uint16_t>(data[2]) << 8),
            static_cast<uint16_t>(data[3]) |
                (static_cast<uint16_t>(data[4]) << 8),
            false);
    }
}

}  // namespace

bool sensor_subscribe_plx(NimBLEClient *client) {
    if (!client) return false;
    NimBLERemoteService *plx_service =
        client->getService(NimBLEUUID(PLX_SERVICE_UUID));
    if (!plx_service) return false;

    NimBLERemoteCharacteristic *continuous =
        plx_service->getCharacteristic(NimBLEUUID(PLX_CONTINUOUS_UUID));
    if (continuous && continuous->canNotify() &&
        continuous->subscribe(true, sensor_plx_notify_cb)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor subscribed PLX continuous\n");
        return true;
    }

    NimBLERemoteCharacteristic *spot =
        plx_service->getCharacteristic(NimBLEUUID(PLX_SPOT_UUID));
    if (spot && (spot->canNotify() || spot->canIndicate()) &&
        spot->subscribe(spot->canNotify(), sensor_plx_notify_cb)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor subscribed PLX spot\n");
        return true;
    }

    return false;
}
#endif

}  // namespace aircannect
