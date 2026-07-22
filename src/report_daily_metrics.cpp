#include "report_daily_metrics.h"

#include <math.h>
#include <string.h>

#include "edf_bytes.h"
#include "edf_file_writer.h"
#include "edf_str_record_reader.h"
#include "edf_str_signal_table.h"

namespace aircannect {
namespace {

bool read_str_label_value(const uint8_t *record,
                          size_t len,
                          const char *label,
                          float &out) {
    return edf_str_record_read_physical(record, len, label, 0, out);
}

bool summary_scaled_value(const ReportSummaryRecord &record,
                          ReportSummaryField field,
                          float scale,
                          float &out) {
    uint32_t raw = 0;
    if (!report_summary_field_value(record, field, raw)) return false;
    out = static_cast<float>(raw) * scale;
    return isfinite(out);
}

}  // namespace

bool report_daily_metrics_any(const ReportDailyMetrics &metrics) {
    return metrics.has_ahi ||
           metrics.has_oa_index ||
           metrics.has_ca_index ||
           metrics.has_ua_index ||
           metrics.has_hypopnea_index ||
           metrics.has_arousal_index ||
           metrics.has_mask_pressure_50 ||
           metrics.has_leak_50;
}

bool report_daily_metrics_from_summary(const ReportSummaryRecord &record,
                                       ReportDailyMetrics &out) {
    out = ReportDailyMetrics();
    out.source = ReportMetricSource::Summary;

    out.has_ahi = record.has_ahi;
    out.ahi = record.ahi / 10.0f;
    out.has_oa_index = record.has_oa_index;
    out.oa_index = record.oa_index / 10.0f;
    out.has_ca_index = record.has_ca_index;
    out.ca_index = record.ca_index / 10.0f;
    out.has_ua_index = record.has_ua_index;
    out.ua_index = record.ua_index / 10.0f;
    out.has_hypopnea_index = record.has_hypopnea_index;
    out.hypopnea_index = record.hypopnea_index / 10.0f;
    out.has_arousal_index = record.has_rera_index;
    out.arousal_index = record.rera_index / 10.0f;

    out.has_mask_pressure_50 =
        summary_scaled_value(record,
                             ReportSummaryField::MaskPressureMedian,
                             1.0f / 100.0f,
                             out.mask_pressure_50_cm_h2o);
    out.has_leak_50 =
        summary_scaled_value(record,
                             ReportSummaryField::LeakMedian,
                             1.0f / 100.0f,
                             out.leak_50_l_min);
    return report_daily_metrics_any(out);
}

bool report_daily_metrics_from_str_record(const uint8_t *record,
                                          size_t len,
                                          ReportDailyMetrics &out) {
    out = ReportDailyMetrics();
    out.source = ReportMetricSource::StrEdf;

    out.has_ahi = read_str_label_value(record, len, "AHI", out.ahi);
    out.has_hypopnea_index =
        read_str_label_value(record, len, "HI", out.hypopnea_index);
    out.has_oa_index =
        read_str_label_value(record, len, "OAI", out.oa_index);
    out.has_ca_index =
        read_str_label_value(record, len, "CAI", out.ca_index);
    out.has_ua_index =
        read_str_label_value(record, len, "UAI", out.ua_index);
    out.has_arousal_index =
        read_str_label_value(record, len, "RIN", out.arousal_index);
    out.has_mask_pressure_50 =
        read_str_label_value(record,
                             len,
                             "MaskPress.50",
                             out.mask_pressure_50_cm_h2o);
    if (read_str_label_value(record, len, "Leak.50", out.leak_50_l_min)) {
        out.leak_50_l_min *= 60.0f;
        out.has_leak_50 = true;
    }
    float duration_min = 0.0f;
    if (read_str_label_value(record, len, "Duration", duration_min) &&
        duration_min >= 0.0f) {
        out.has_duration_min = true;
        out.duration_min = static_cast<uint32_t>(duration_min + 0.5f);
    }
    return report_daily_metrics_any(out);
}

}  // namespace aircannect
