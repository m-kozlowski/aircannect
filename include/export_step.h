#pragma once

#include <stdint.h>

namespace aircannect {

enum class ExportStep : uint8_t {
    Idle,
    Working,
    Waiting,
};

}  // namespace aircannect
