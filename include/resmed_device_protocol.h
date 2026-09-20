#pragma once

#include <string>

#include "resmed_device_model.h"

namespace aircannect {

struct ResmedIdentityQuery {
    const char *product_name;
    const char *serial_number;
    const char *software_identifier;
    const char *bootloader_identifier;
    const char *platform_id;
    const char *variant_id;

    std::string params_json() const;
};

struct ResmedRuntimeQuery {
    const char *therapy_profile;
    const char *running_mode;
    const char *running_mode_alias;
    // Null when running_mode itself reports the current therapy state.
    const char *therapy_state;

    std::string params_json() const;
};

struct ResmedDeviceProtocol {
    ResmedRuntimeQuery runtime;
    // Null means that the device has no timezone readout.
    const char *timezone;

    // Extra read alongside the active mode, TherapyProfiles and FeatureProfiles.
    const char *settings_extra_field;
    // Null when the device has no catalog of custom settings.
    const char *settings_extensions;
};

extern const ResmedIdentityQuery RESMED_IDENTITY;
extern const char RESMED_MOTOR_RUNTIME[];

const ResmedDeviceProtocol *resmed_device_protocol(ResmedDeviceModel model);

}  // namespace aircannect
