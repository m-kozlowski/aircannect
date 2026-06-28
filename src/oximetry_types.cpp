#include "oximetry_types.h"

#include <ctype.h>
#include <string>

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

bool parse_oximetry_advertise_mode(const char *value,
                                   OximetryAdvertiseMode &mode) {
    if (!value) return false;
    const char *start = value;
    while (*start && isspace(static_cast<unsigned char>(*start))) start++;
    const char *end = start;
    while (*end) end++;
    while (end > start &&
           isspace(static_cast<unsigned char>(*(end - 1)))) {
        end--;
    }

    std::string normalized;
    normalized.reserve(static_cast<size_t>(end - start));
    for (const char *p = start; p < end; ++p) {
        normalized.push_back(static_cast<char>(
            tolower(static_cast<unsigned char>(*p))));
    }

    if (normalized == "auto" || normalized == "automatic") {
        mode = OximetryAdvertiseMode::Auto;
        return true;
    }
    if (normalized == "manual" || normalized == "on-demand" ||
        normalized == "ondemand") {
        mode = OximetryAdvertiseMode::Manual;
        return true;
    }
    return false;
}

#ifdef ARDUINO
bool parse_oximetry_advertise_mode(String value,
                                   OximetryAdvertiseMode &mode) {
    return parse_oximetry_advertise_mode(value.c_str(), mode);
}
#endif

}  // namespace aircannect
