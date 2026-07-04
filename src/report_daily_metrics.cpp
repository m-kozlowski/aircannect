#include "report_daily_metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "edf_bytes.h"
#include "edf_file_writer.h"
#include "edf_str_signal_table.h"

#if defined(ARDUINO)
#include "calendar_utils.h"
#include "edf_str_file_layout.h"
#include "report_night_index.h"
#include "storage_manager.h"
#endif

namespace aircannect {
namespace {

constexpr int16_t STR_MISSING_DIGITAL = -1;

bool parse_float_field(const char *text, float &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const float value = strtof(text, &end);
    if (end == text || !isfinite(value)) return false;
    out = value;
    return true;
}

bool decode_physical_sample(const EdfSignalSpec &spec,
                            int16_t digital,
                            float &out) {
    if (digital == STR_MISSING_DIGITAL) return false;
    if (digital < spec.digital_min_value ||
        digital > spec.digital_max_value) {
        return false;
    }

    float physical_min = 0.0f;
    float physical_max = 0.0f;
    if (!parse_float_field(spec.physical_min, physical_min) ||
        !parse_float_field(spec.physical_max, physical_max)) {
        return false;
    }

    const float digital_span =
        static_cast<float>(spec.digital_max_value - spec.digital_min_value);
    if (digital_span == 0.0f) return false;
    const float physical_span = physical_max - physical_min;
    out = physical_min +
          (static_cast<float>(digital - spec.digital_min_value) *
           physical_span) /
              digital_span;
    return isfinite(out);
}

bool read_str_label_value(const uint8_t *record,
                          size_t len,
                          const char *label,
                          float &out) {
    if (!record || !label || len < edf_str_record_size()) return false;
    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *descriptor =
            edf_str_signal_descriptor(i);
        if (!descriptor || !descriptor->spec.label ||
            strcmp(descriptor->spec.label, label) != 0) {
            continue;
        }

        const size_t offset = edf_str_signal_sample_offset(i);
        if (offset >= AC_EDF_STR_SAMPLES_PER_RECORD) return false;
        const int16_t digital = edf_read_i16_le_sample(record, offset);
        return decode_physical_sample(descriptor->spec, digital, out);
    }
    return false;
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
bool parse_report_sleep_day(const char *sleep_day,
                            int &year,
                            unsigned &month,
                            unsigned &day) {
    if (!sleep_day || strlen(sleep_day) != 8) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (sleep_day[i] < '0' || sleep_day[i] > '9') return false;
    }

    char buf[5] = {};
    memcpy(buf, sleep_day, 4);
    year = static_cast<int>(strtol(buf, nullptr, 10));

    buf[0] = sleep_day[4];
    buf[1] = sleep_day[5];
    buf[2] = '\0';
    month = static_cast<unsigned>(strtoul(buf, nullptr, 10));

    buf[0] = sleep_day[6];
    buf[1] = sleep_day[7];
    day = static_cast<unsigned>(strtoul(buf, nullptr, 10));

    return year > 0 &&
           month >= 1 && month <= 12 &&
           day >= 1 &&
           day <= calendar_days_in_month(year, static_cast<int>(month));
}

bool report_sleep_day_date_sample(const ReportSummaryRecord &night,
                                  int16_t &out) {
    char sleep_day[9] = {};
    if (!report_summary_sleep_day_yyyymmdd(night,
                                           sleep_day,
                                           sizeof(sleep_day))) {
        return false;
    }

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!parse_report_sleep_day(sleep_day, year, month, day)) return false;

    const int64_t days = calendar_days_from_civil(year, month, day);
    if (days < INT16_MIN || days > INT16_MAX) return false;

    out = static_cast<int16_t>(days);
    return true;
}

bool read_str_record_date(File &file,
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

bool read_str_record(File &file,
                     uint32_t record_index,
                     uint8_t *record,
                     size_t record_size) {
    if (!record || record_size != edf_str_record_size()) return false;
    const size_t offset = edf_str_record_offset(record_index);
    if (!file.seek(offset)) return false;
    return file.read(record, record_size) == static_cast<int>(record_size);
}

bool find_str_record_by_date(File &file,
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

    Storage::Guard guard;
    File file = Storage::open("/STR.edf", "r");
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
