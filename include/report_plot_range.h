#pragma once

#include <stdint.h>

namespace aircannect {
namespace report_manager_internal {

struct PlotRange {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

}  // namespace report_manager_internal
}  // namespace aircannect
