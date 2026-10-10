#include "report_daily_metrics.h"
#include "report_daily_statistics.h"

#include <math.h>
#include <string.h>

#include "edf_bytes.h"
#include "edf_file_writer.h"
#include "edf_str_record_reader.h"
#include "edf_str_signal_table.h"

namespace aircannect {
namespace {

void read_str_statistics(const uint8_t *record, size_t length,
                         ReportDailyStatistics &out);

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
    out.ahi = record.ahi;
    out.has_oa_index = record.has_oa_index;
    out.oa_index = record.oa_index;
    out.has_ca_index = record.has_ca_index;
    out.ca_index = record.ca_index;
    out.has_ua_index = record.has_ua_index;
    out.ua_index = record.ua_index;
    out.has_hypopnea_index = record.has_hypopnea_index;
    out.hypopnea_index = record.hypopnea_index;
    out.has_arousal_index = record.has_rera_index;
    out.arousal_index = record.rera_index;

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
                                          ReportDailyMetrics &out,
                                          ReportDailyStatistics &statistics) {
    out = ReportDailyMetrics();
    out.source = ReportMetricSource::StrEdf;
    read_str_statistics(record, len, statistics);

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

    using Stats = ReportDailyStatistics;
    const struct {
        Stats::Signal signal;
        Stats::Field field;
        bool &present;
        float &value;
    } retained[] = {
        {Stats::MaskPressure, Stats::Median, out.has_mask_pressure_50, out.mask_pressure_50_cm_h2o},
        {Stats::MaskPressure, Stats::P95, out.has_mask_pressure_95, out.mask_pressure_95_cm_h2o},
        {Stats::Leak, Stats::Median, out.has_leak_50, out.leak_50_l_min},
        {Stats::Leak, Stats::P95, out.has_leak_95, out.leak_95_l_min},
        {Stats::MinuteVentilation, Stats::Median, out.has_minute_ventilation_50, out.minute_ventilation_50_l_min},
        {Stats::MinuteVentilation, Stats::P95, out.has_minute_ventilation_95, out.minute_ventilation_95_l_min},
        {Stats::RespiratoryRate, Stats::Median, out.has_respiratory_rate_50, out.respiratory_rate_50_bpm},
        {Stats::RespiratoryRate, Stats::P95, out.has_respiratory_rate_95, out.respiratory_rate_95_bpm},
        {Stats::TidalVolume, Stats::Median, out.has_tidal_volume_50, out.tidal_volume_50_l},
        {Stats::TidalVolume, Stats::P95, out.has_tidal_volume_95, out.tidal_volume_95_l},
        {Stats::Spo2, Stats::Median, out.has_spo2_50, out.spo2_50_percent},
    };
    for (const auto &field : retained) {
        const auto &row = statistics.rows[field.signal];
        field.present = (row.valid_mask & (1u << field.field)) != 0;
        field.value = row.values[field.field] / 1000.0f;
    }

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

namespace {

void read_str_statistics(const uint8_t *record, size_t length,
                         ReportDailyStatistics &out) {
    out = {};
    static constexpr const char *labels[][ReportDailyStatistics::FieldCount] = {
        {"MaskPress.50", "MaskPress.95", "MaskPress.Max"},
        {"TgtIPAP.50", "TgtIPAP.95", "TgtIPAP.Max"},
        {"TgtEPAP.50", "TgtEPAP.95", "TgtEPAP.Max"},
        {"Leak.50", "Leak.95", "Leak.Max"},
        {"MinVent.50", "MinVent.95", "MinVent.Max"},
        {"RespRate.50", "RespRate.95", "RespRate.Max"},
        {"TidVol.50", "TidVol.95", "TidVol.Max"},
        {"SpO2.50", "SpO2.95", "SpO2.Max"},
        {"TgtVent.50", "TgtVent.95", "TgtVent.Max"},
        {"Ti.50", "Ti.95", "Ti.Max"},
        {"IERatio.50", "IERatio.95", "IERatio.Max"},
    };
    static_assert(sizeof(labels) / sizeof(labels[0]) == ReportDailyStatistics::Count);

    for (size_t i = 0; i < ReportDailyStatistics::Count; ++i) {
        const float scale = i == ReportDailyStatistics::Leak ? 60000.0f : 1000.0f;
        for (size_t j = 0; j < ReportDailyStatistics::FieldCount; ++j) {
            float value = 0;
            if (!read_str_label_value(record, length, labels[i][j], value)) continue;

            out.rows[i].values[j] = static_cast<int32_t>(lroundf(value * scale));
            out.rows[i].valid_mask |= 1u << j;
        }
    }
}

}  // namespace

}  // namespace aircannect
