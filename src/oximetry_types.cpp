#include "oximetry_types.h"

#include "string_util.h"

namespace aircannect {

const char *oximetry_advertise_mode_name(OximetryAdvertiseMode mode) {
    switch (mode) {
        case OximetryAdvertiseMode::Auto: return "auto";
        case OximetryAdvertiseMode::Manual: return "manual";
        default: return "auto";
    }
}

bool oximetry_advertise_mode_valid(OximetryAdvertiseMode mode) {
    return mode == OximetryAdvertiseMode::Auto ||
           mode == OximetryAdvertiseMode::Manual;
}

bool parse_oximetry_advertise_mode(String value,
                                   OximetryAdvertiseMode &mode) {
    trim_inplace(value);
    to_lower_inplace(value);
    if (value == "auto" || value == "automatic") {
        mode = OximetryAdvertiseMode::Auto;
        return true;
    }
    if (value == "manual" || value == "on-demand" || value == "ondemand") {
        mode = OximetryAdvertiseMode::Manual;
        return true;
    }
    return false;
}

}  // namespace aircannect
