#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace aircannect {

enum class OximetryAdvertiseMode : uint8_t {
    Auto = 0,
    Manual = 1,
};

const char *oximetry_advertise_mode_name(OximetryAdvertiseMode mode);
bool parse_oximetry_advertise_mode(String value,
                                   OximetryAdvertiseMode &mode);
bool oximetry_advertise_mode_valid(OximetryAdvertiseMode mode);

}  // namespace aircannect
