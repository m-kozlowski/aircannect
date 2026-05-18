#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace aircannect {

enum class SoftApMode : uint8_t {
    Auto = 0,
    Forced = 1,
};

inline const char *softap_mode_name(SoftApMode mode) {
    switch (mode) {
        case SoftApMode::Forced: return "forced";
        case SoftApMode::Auto:
        default: return "auto";
    }
}

inline bool softap_mode_valid(SoftApMode mode) {
    return mode == SoftApMode::Auto || mode == SoftApMode::Forced;
}

inline bool parse_softap_mode(String value, SoftApMode &mode) {
    value.trim();
    value.toLowerCase();
    if (value == "auto" || value == "fallback" || value == "0") {
        mode = SoftApMode::Auto;
        return true;
    }
    if (value == "forced" || value == "force" || value == "always" ||
        value == "sta_ap" || value == "sta+ap" || value == "1") {
        mode = SoftApMode::Forced;
        return true;
    }
    return false;
}

}  // namespace aircannect
