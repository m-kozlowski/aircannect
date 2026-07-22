#include "report_daily_metrics.h"

#include <math.h>
#include <string.h>

#include "edf_bytes.h"
#include "edf_file_writer.h"
#include "edf_str_record_reader.h"
#include "edf_str_signal_table.h"

#if defined(ARDUINO)
#include "calendar_utils.h"
#include "edf_str_file_layout.h"
#include "report_night_index.h"
#include "report_legacy_storage.h"
#endif

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

#if defined(ARDUINO)
bool report_sleep_day_date_sample(const ReportSummaryRecord &night,
                                  int16_t &out) {
    char sleep_day[9] = {};
    if (!report_summary_sleep_day_yyyymmdd(night,
                                           sleep_day,
                                           sizeof(sleep_day))) {
        return false;
    }

    int64_t days = 0;
    if (!calendar_yyyymmdd_to_days(sleep_day, days)) return false;
    if (days < INT16_MIN || days > INT16_MAX) return false;

    out = static_cast<int16_t>(days);
    return true;
}

bool read_str_record_date(ReportLegacyFile &file,
                          uint32_t record_index,
                          int16_t &date_sample) {
    uint8_t raw[2] = {};
    const size_t offset = edf_str_record_offset(record_index);
    if (!file.seek(offset)) return false;
    if (file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
        return false;
    }
    date_sample = edf_str_record_date_sample(raw, sizeof(raw));
    return true;
}

bool read_str_record(ReportLegacyFile &file,
                     uint32_t record_index,
                     uint8_t *record,
                     size_t record_size) {
    if (!record || record_size != edf_str_record_size()) return false;
    const size_t offset = edf_str_record_offset(record_index);
    if (!file.seek(offset)) return false;
    return file.read(record, record_size) == static_cast<int>(record_size);
}

bool find_str_record_by_date(ReportLegacyFile &file,
                             uint32_t record_count,
                             int16_t target_date,
                             uint32_t &record_index) {
    uint32_t low = 0;
    uint32_t high = record_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        int16_t mid_date = -1;
        if (!read_str_record_date(file, mid, mid_date)) return false;
        if (mid_date < target_date) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    if (low >= record_count) return false;

    int16_t found_date = -1;
    if (!read_str_record_date(file, low, found_date)) return false;
    if (found_date != target_date) return false;

    record_index = low;
    return true;
}
#endif

}  // namespace

const char *report_metric_source_name(ReportMetricSource source) {
    switch (source) {
        case ReportMetricSource::StrEdf: return "str_edf";
        case ReportMetricSource::Summary: return "summary";
        case ReportMetricSource::Calculated: return "calculated";
        case ReportMetricSource::None:
        default:
            return "none";
    }
}

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

bool report_daily_metrics_from_str_file(const ReportSummaryRecord &record,
                                        uint32_t duration_tolerance_min,
                                        ReportDailyMetrics &out) {
#if !defined(ARDUINO)
    (void)record;
    (void)duration_tolerance_min;
    out = ReportDailyMetrics();
    return false;
#else
    out = ReportDailyMetrics();

    int16_t target_date = 0;
    if (!report_sleep_day_date_sample(record, target_date)) return false;

    ReportLegacyStorageGuard guard;
    ReportLegacyFile file = ReportLegacyStorage::open("/STR.edf", "r");
    if (!file || file.isDirectory()) return false;

    EdfStrFileLayout layout;
    if (!edf_str_file_layout_from_size(static_cast<size_t>(file.size()),
                                       layout) ||
        layout.record_count == 0) {
        file.close();
        return false;
    }

    uint8_t str_record[AC_EDF_STR_SAMPLES_PER_RECORD * 2] = {};
    const size_t str_record_size = edf_str_record_size();
    uint32_t record_index = 0;
    if (!find_str_record_by_date(file,
                                 layout.record_count,
                                 target_date,
                                 record_index) ||
        !read_str_record(file, record_index, str_record, str_record_size)) {
        file.close();
        return false;
    }
    file.close();

    if (!report_daily_metrics_from_str_record(str_record,
                                              str_record_size,
                                              out)) {
        return false;
    }

    if (record.duration_min > 0 && out.has_duration_min) {
        const uint32_t expected = static_cast<uint32_t>(record.duration_min);
        const uint32_t actual = out.duration_min;
        const uint32_t delta =
            expected > actual ? expected - actual : actual - expected;
        if (delta > duration_tolerance_min) return false;
    }

    return true;
#endif
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
