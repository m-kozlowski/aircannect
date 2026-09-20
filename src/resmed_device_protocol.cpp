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
    // ROP and PHI have no registered long names in the AS11 DataItem map.
    {"ActiveTherapyProfile", "_ROP", nullptr},
    "TimeZoneOffset",
    "_PHI", "AirbreakInfo",
    true, "Cpap",
    "IdentificationProfiles",
    0, 0,
};

const ResmedDeviceProtocol AIRMINI = {
    {"TherapyMode", "_RUNNING_MODE_REQUEST", "FGState"},
    nullptr,
    "MaskType", nullptr,
    false, "CPAP",
    nullptr,
    40, 200,
};

}  // namespace

std::string ResmedIdentityQuery::params_json() const {
    return build_get_params({product_name, serial_number, software_identifier,
                             bootloader_identifier, platform_id, variant_id});
}

std::string ResmedRuntimeQuery::params_json() const {
    return build_get_params({therapy_profile, running_mode, therapy_state});
}

std::string ResmedDeviceProtocol::identification_params_json() const {
    if (identification_profiles) return build_get_params({identification_profiles});

    const ResmedIdentityQuery &identity = RESMED_IDENTITY;
    return build_get_params({identity.product_name, identity.serial_number,
                             identity.software_identifier,
                             identity.bootloader_identifier,
                             identity.platform_id, identity.variant_id,
                             "ProductCode"});
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
