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
    float physical = 0.0f;
    if (!report_summary_field_physical_value(record, field, physical)) {
        return false;
    }
    out = physical * scale;
    return isfinite(out);
}

bool summary_uint_value(const ReportSummaryRecord &record,
                        ReportSummaryField field,
                        uint32_t &out) {
    return report_summary_field_value(record, field, out);
}

bool read_str_minutes(const uint8_t *record,
                      size_t len,
                      const char *label,
                      uint32_t &out) {
    float value = 0.0f;
    if (!read_str_label_value(record, len, label, value) ||
        value < 0.0f || value > static_cast<float>(UINT32_MAX)) {
        return false;
    }

    out = static_cast<uint32_t>(value + 0.5f);
    return true;
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
           metrics.has_mask_pressure_95 ||
           metrics.has_leak_50 ||
           metrics.has_leak_95 ||
           metrics.has_minute_ventilation_50 ||
           metrics.has_minute_ventilation_95 ||
           metrics.has_respiratory_rate_50 ||
           metrics.has_respiratory_rate_95 ||
           metrics.has_tidal_volume_50 ||
           metrics.has_tidal_volume_95 ||
           metrics.has_spo2_50 ||
           metrics.has_spo2_threshold_minutes ||
           metrics.has_csr_minutes;
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
                             1.0f,
                             out.mask_pressure_50_cm_h2o);
    out.has_mask_pressure_95 =
        summary_scaled_value(record,
                             ReportSummaryField::MaskPressure95,
                             1.0f,
                             out.mask_pressure_95_cm_h2o);
    out.has_leak_50 =
        summary_scaled_value(record,
                             ReportSummaryField::LeakMedian,
                             60.0f,
                             out.leak_50_l_min);
    out.has_leak_95 =
        summary_scaled_value(record,
                             ReportSummaryField::Leak95,
                             60.0f,
                             out.leak_95_l_min);
    out.has_minute_ventilation_50 =
        summary_scaled_value(record,
                             ReportSummaryField::MinuteVentMedian,
                             1.0f,
                             out.minute_ventilation_50_l_min);
    out.has_minute_ventilation_95 =
        summary_scaled_value(record,
                             ReportSummaryField::MinuteVent95,
                             1.0f,
                             out.minute_ventilation_95_l_min);
    out.has_respiratory_rate_50 =
        summary_scaled_value(record,
                             ReportSummaryField::RespiratoryRateMedian,
                             1.0f,
                             out.respiratory_rate_50_bpm);
    out.has_respiratory_rate_95 =
        summary_scaled_value(record,
                             ReportSummaryField::RespiratoryRate95,
                             1.0f,
                             out.respiratory_rate_95_bpm);
    out.has_tidal_volume_50 =
        summary_scaled_value(record,
                             ReportSummaryField::TidalVolumeMedian,
                             1.0f,
                             out.tidal_volume_50_l);
    out.has_tidal_volume_95 =
        summary_scaled_value(record,
                             ReportSummaryField::TidalVolume95,
                             1.0f,
                             out.tidal_volume_95_l);
    out.has_spo2_50 =
        summary_scaled_value(record,
                             ReportSummaryField::Spo2Median,
                             1.0f,
                             out.spo2_50_percent);
    out.has_spo2_threshold_minutes = summary_uint_value(
        record,
        ReportSummaryField::Spo2ThresholdMinutes,
        out.spo2_threshold_minutes);
    out.has_csr_minutes = summary_uint_value(
        record, ReportSummaryField::Csr, out.csr_minutes);
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
    out.has_mask_pressure_95 =
        read_str_label_value(record,
                             len,
                             "MaskPress.95",
                             out.mask_pressure_95_cm_h2o);
    if (read_str_label_value(record, len, "Leak.50", out.leak_50_l_min)) {
        out.leak_50_l_min *= 60.0f;
        out.has_leak_50 = true;
    }
    if (read_str_label_value(record, len, "Leak.95", out.leak_95_l_min)) {
        out.leak_95_l_min *= 60.0f;
        out.has_leak_95 = true;
    }
    out.has_minute_ventilation_50 = read_str_label_value(
        record, len, "MinVent.50", out.minute_ventilation_50_l_min);
    out.has_minute_ventilation_95 = read_str_label_value(
        record, len, "MinVent.95", out.minute_ventilation_95_l_min);
    out.has_respiratory_rate_50 = read_str_label_value(
        record, len, "RespRate.50", out.respiratory_rate_50_bpm);
    out.has_respiratory_rate_95 = read_str_label_value(
        record, len, "RespRate.95", out.respiratory_rate_95_bpm);
    out.has_tidal_volume_50 = read_str_label_value(
        record, len, "TidVol.50", out.tidal_volume_50_l);
    out.has_tidal_volume_95 = read_str_label_value(
        record, len, "TidVol.95", out.tidal_volume_95_l);
    out.has_spo2_50 = read_str_label_value(
        record, len, "SpO2.50", out.spo2_50_percent);
    out.has_spo2_threshold_minutes = read_str_minutes(
        record, len, "SpO2Thresh", out.spo2_threshold_minutes);
    out.has_csr_minutes = read_str_minutes(
        record, len, "CSR", out.csr_minutes);

    float duration_min = 0.0f;
    if (read_str_label_value(record, len, "Duration", duration_min) &&
        duration_min >= 0.0f) {
        out.has_duration_min = true;
        out.duration_min = static_cast<uint32_t>(duration_min + 0.5f);
    }
    return report_daily_metrics_any(out);
}

}  // namespace aircannect
