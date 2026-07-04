#pragma once

#include <stdint.h>

#include "report_daily_metrics.h"
#include "report_result_types.h"

namespace aircannect {

struct ReportMetricAverage {
    double sum = 0.0;
    uint32_t count = 0;

    void add(float value);
    bool mean(float &out) const;
};

void report_apply_daily_metrics_to_result_status(
    ReportResultStatus &status,
    const ReportDailyMetrics &metrics);

}  // namespace aircannect
