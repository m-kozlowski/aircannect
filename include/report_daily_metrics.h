#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_proto.h"

namespace aircannect {

enum class ReportMetricSource : uint8_t {
    None,
    StrEdf,
    Summary,
    Calculated,
};

struct ReportDailyMetrics {
    ReportMetricSource source = ReportMetricSource::None;

    bool has_ahi = false;
    bool has_oa_index = false;
    bool has_ca_index = false;
    bool has_ua_index = false;
    bool has_hypopnea_index = false;
    bool has_arousal_index = false;
    bool has_mask_pressure_50 = false;
    bool has_leak_50 = false;
    bool has_duration_min = false;

    float ahi = 0.0f;
    float oa_index = 0.0f;
    float ca_index = 0.0f;
    float ua_index = 0.0f;
    float hypopnea_index = 0.0f;
    float arousal_index = 0.0f;
    float mask_pressure_50_cm_h2o = 0.0f;
    float leak_50_l_min = 0.0f;
    uint32_t duration_min = 0;
};

const char *report_metric_source_name(ReportMetricSource source);
bool report_daily_metrics_any(const ReportDailyMetrics &metrics);
bool report_daily_metrics_from_summary(const ReportSummaryRecord &record,
                                       ReportDailyMetrics &out);
bool report_daily_metrics_from_str_record(const uint8_t *record,
                                          size_t len,
                                          ReportDailyMetrics &out);

}  // namespace aircannect
