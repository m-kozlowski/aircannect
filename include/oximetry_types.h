#pragma once

#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace aircannect {

enum class OximetryAdvertiseMode : uint8_t {
    Auto = 0,
    Manual = 1,
};

const char *oximetry_advertise_mode_name(OximetryAdvertiseMode mode);
bool parse_oximetry_advertise_mode(const char *value,
                                   OximetryAdvertiseMode &mode);
#ifdef ARDUINO
bool parse_oximetry_advertise_mode(String value,
                                   OximetryAdvertiseMode &mode);
#endif
bool oximetry_advertise_mode_valid(OximetryAdvertiseMode mode);

}  // namespace aircannect
