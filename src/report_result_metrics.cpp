#include "report_result_metrics.h"

#include <math.h>

namespace aircannect {

void ReportMetricAverage::add(float value) {
    if (!isfinite(value)) return;

    sum += static_cast<double>(value);
    count++;
}

bool ReportMetricAverage::mean(float &out) const {
    if (count == 0) return false;

    out = static_cast<float>(sum / static_cast<double>(count));
    return isfinite(out);
}

void report_apply_daily_metrics_to_result_status(
    ReportResultStatus &status,
    const ReportDailyMetrics &metrics) {
    if (metrics.has_ahi && !status.ahi_valid) {
        status.ahi = metrics.ahi;
        status.ahi_valid = true;
        status.ahi_source = metrics.source;
    }
    if (metrics.has_oa_index && !status.oa_index_valid) {
        status.oa_index = metrics.oa_index;
        status.oa_index_valid = true;
        status.oa_index_source = metrics.source;
    }
    if (metrics.has_ca_index && !status.ca_index_valid) {
        status.ca_index = metrics.ca_index;
        status.ca_index_valid = true;
        status.ca_index_source = metrics.source;
    }
    if (metrics.has_ua_index && !status.ua_index_valid) {
        status.ua_index = metrics.ua_index;
        status.ua_index_valid = true;
        status.ua_index_source = metrics.source;
    }
    if (metrics.has_hypopnea_index && !status.hypopnea_index_valid) {
        status.hypopnea_index = metrics.hypopnea_index;
        status.hypopnea_index_valid = true;
        status.hypopnea_index_source = metrics.source;
    }
    if (metrics.has_arousal_index && !status.arousal_index_valid) {
        status.arousal_index = metrics.arousal_index;
        status.arousal_index_valid = true;
        status.arousal_index_source = metrics.source;
    }
    if (metrics.has_mask_pressure_50 && !status.mask_pressure_50_valid) {
        status.mask_pressure_50_cm_h2o = metrics.mask_pressure_50_cm_h2o;
        status.mask_pressure_50_valid = true;
        status.mask_pressure_50_source = metrics.source;
    }
    if (metrics.has_leak_50 && !status.leak_50_valid) {
        status.leak_50_l_min = metrics.leak_50_l_min;
        status.leak_50_valid = true;
        status.leak_50_source = metrics.source;
    }
}

}  // namespace aircannect
