#pragma once

#include <stdint.h>
#include <string>

#include "spool_client.h"

namespace aircannect {

enum class ReportSummaryState : uint8_t {
    Idle,
    Fetching,
    Ready,
    Error,
};

struct ReportSummaryStatus {
    ReportSummaryState state = ReportSummaryState::Idle;
    uint32_t revision = 0;
    uint32_t records_total = 0;
    uint32_t nights_with_therapy = 0;
    uint32_t elapsed_ms = 0;
    std::string active_spool;
    std::string error;
    SpoolClientStatus spool;
};

}  // namespace aircannect
