#pragma once

#include <stdint.h>

namespace aircannect {

enum class ReportPlotBuildPhase : uint8_t {
    Idle,
    Events,
    Series,
};

}  // namespace aircannect
