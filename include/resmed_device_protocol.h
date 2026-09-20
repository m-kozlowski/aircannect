#pragma once

#include "resmed_device_model.h"

namespace aircannect {

struct ResmedIdentityQuery {
    const char *params_json;
    const char *product_name;
    const char *serial_number;
    const char *software_identifier;
    const char *bootloader_identifier;
    const char *platform_id;
    const char *variant_id;
};

struct ResmedRuntimeQuery {
    const char *params_json;
    const char *therapy_profile;
    const char *running_mode;
    const char *running_mode_alias;
    // Null when running_mode itself reports the current therapy state.
    const char *therapy_state;
};

struct ResmedSingleValueQuery {
    // Null parameters mean that the device does not support this query.
    const char *params_json;
    const char *field;
};

struct ResmedDeviceProtocol {
    ResmedIdentityQuery identity;
    ResmedRuntimeQuery runtime;
    ResmedSingleValueQuery motor_runtime;
    ResmedSingleValueQuery timezone;
};

extern const char RESMED_PLATFORM_GET_PARAMS[];
extern const char RESMED_PLATFORM_FIELD[];

const ResmedDeviceProtocol *resmed_device_protocol(ResmedDeviceModel model);

}  // namespace aircannect
