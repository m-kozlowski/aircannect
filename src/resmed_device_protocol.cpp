#include "resmed_device_protocol.h"

#include "as11_rpc.h"

namespace aircannect {

const ResmedIdentityQuery RESMED_IDENTITY = {
    "ProductName", "SerialNumber", "ApplicationIdentifier",
    "BootloaderIdentifier", "PlatformIdentifier", "VariantIdentifier",
};
const char RESMED_MOTOR_RUNTIME[] = "MotorRunMeter";

namespace {

const ResmedDeviceProtocol AIRSENSE11 = {
    {"ActiveTherapyProfile", "_ROP", "ROP", nullptr},
    "TimeZoneOffset",
    "_PHI", "AirbreakInfo",
};

const ResmedDeviceProtocol AIRMINI = {
    {"TherapyMode", "_RUNNING_MODE_REQUEST", nullptr, "FGState"},
    nullptr,
    "MaskType", nullptr,
};

}  // namespace

std::string ResmedIdentityQuery::params_json() const {
    return build_get_params({product_name, serial_number, software_identifier,
                             bootloader_identifier, platform_id, variant_id});
}

std::string ResmedRuntimeQuery::params_json() const {
    return build_get_params({therapy_profile, running_mode, therapy_state});
}

const ResmedDeviceProtocol *resmed_device_protocol(ResmedDeviceModel model) {
    switch (model) {
        case ResmedDeviceModel::AirSense11: return &AIRSENSE11;
        case ResmedDeviceModel::AirMini: return &AIRMINI;
        case ResmedDeviceModel::Unknown: return nullptr;
    }
    return nullptr;
}

}  // namespace aircannect
