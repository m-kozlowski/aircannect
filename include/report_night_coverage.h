#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_limits.h"
#include "report_sources.h"

namespace aircannect {

struct ReportNightSourceCoverage {
    ReportSourceId source = ReportSourceId::Summary;
    bool required = false;
    bool complete = false;
};

struct ReportNightCoverageStatus {
    bool found = false;
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    uint32_t duration_min = 0;
    uint32_t missing_required = 0;
    size_t source_count = 0;
    ReportNightSourceCoverage sources[AC_REPORT_NIGHT_SOURCE_MAX];
};

}  // namespace aircannect
