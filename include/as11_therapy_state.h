#pragma once

#include <stdint.h>

namespace aircannect {

enum class As11TherapyState : uint8_t {
    Unknown,
    Standby,
    Running,
    Other,
};

enum class As11TherapyTarget : uint8_t {
    None,
    Standby,
    Running,
};

}  // namespace aircannect
