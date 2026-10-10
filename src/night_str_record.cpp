#include "night_str_record.h"

#include "edf_str_record_reader.h"
#include "edf_str_signal_table.h"
#include "as11_settings.h"
#include <math.h>
#include <string.h>

namespace aircannect {
namespace {

constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);

uint64_t record_identity(const uint8_t *record, size_t len) {
    uint64_t hash = FNV_OFFSET;
    for (size_t i = 0; i < len; ++i) {
        hash ^= record[i];
        hash *= FNV_PRIME;
    }
    return hash == 0 ? 1 : hash;
}

bool read_minute(const uint8_t *record,
                 size_t len,
                 const char *label,
                 size_t index,
                 uint16_t &out) {
    int16_t value = 0;
    if (!edf_str_record_read_digital(record, len, label, index, value) ||
        value < 0 || value > 1440) {
        return false;
    }

    out = static_cast<uint16_t>(value);
    return true;
}

void read_settings(const uint8_t *record, size_t len,
                   ReportTherapySettings &out) {
    int16_t mode = 0;
    if (!edf_str_record_read_digital(record, len, "Mode", 0, mode) ||
        mode < 0 || mode >= static_cast<int>(As11SettingsState::MaxModes)) return;

    out.valid_mask = 1;
    out.values[ReportTherapySettings::Mode] = mode * 1000;
    // Profile ownership and applicability come from the settings registry.
    static constexpr const char *fields[] = {
        "TherapyMode", "SetPressure", "MinPressure", "MaxPressure",
        "TargetInspiratoryPressure", "TargetExpiratoryPressure",
        "MaxInspiratoryPressure", "MinExpiratoryPressure", "MaxExpiratoryPressure",
        "SetPressureSupport", "MinPressureSupport", "MaxPressureSupport",
        "SetMinInspiratoryTime", "SetMaxInspiratoryTime", "SetInspiratoryTime",
        "SetRespiratoryRate", "StartPressure",
    };
    static constexpr const char *flat_keys[] = {"EPX", "EPR", "EPT", "RMA", "RMT", "HMS"};
    static_assert(sizeof(fields) / sizeof(fields[0]) == ReportTherapySettings::EprEnabled);
    static_assert(sizeof(flat_keys) / sizeof(flat_keys[0]) ==
                  ReportTherapySettings::Count - ReportTherapySettings::EprEnabled);

    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const auto *signal = edf_str_signal_descriptor(i);
        if (signal->source != EdfStrFieldSource::SettingGet || !signal->short_tag) continue;

        const auto *def = as11_find_setting(signal->short_tag);
        if (!def || !as11_setting_visible_for_mode(*def, mode)) continue;

        size_t field = ReportTherapySettings::Count;
        if (def->source == As11SettingSource::TherapyProfile) {
            for (size_t j = 1; j < ReportTherapySettings::EprEnabled; ++j) {
                if (strcmp(def->source_field, fields[j]) == 0) { field = j; break; }
            }
        } else {
            for (size_t j = 0; j < sizeof(flat_keys) / sizeof(flat_keys[0]); ++j) {
                if (strcmp(signal->short_tag, flat_keys[j]) == 0) {
                    field = ReportTherapySettings::EprEnabled + j;
                    break;
                }
            }
        }
        if (field == ReportTherapySettings::Count) continue;

        float value = 0;
        if (edf_str_record_read_physical(record, len, signal->spec.label, 0, value)) {
            out.values[field] = static_cast<int32_t>(lroundf(value * 1000));
            out.valid_mask |= 1u << field;
        }
    }
}

}  // namespace

bool night_str_record_parse(const uint8_t *record,
                            size_t len,
                            NightStrRecord &out) {
    out = NightStrRecord();
    if (!record) return false;

    int16_t date = 0;
    if (!edf_str_record_read_digital(record, len, "Date", 0, date) ||
        !SleepDayId::from_epoch_days(date, out.sleep_day)) {
        return false;
    }

    int16_t mask_events = 0;
    if (!edf_str_record_read_digital(record,
                                     len,
                                     "MaskEvents",
                                     0,
                                     mask_events) ||
        mask_events < 0 ||
        mask_events > static_cast<int16_t>(AC_NIGHT_STR_MASK_WINDOW_MAX)) {
        return false;
    }

    out.mask_window_count = static_cast<uint8_t>(mask_events);
    for (size_t i = 0; i < out.mask_window_count; ++i) {
        NightStrMaskWindow &window = out.mask_windows[i];
        if (!read_minute(record, len, "MaskOn", i, window.on_minute) ||
            !read_minute(record, len, "MaskOff", i, window.off_minute) ||
            window.off_minute < window.on_minute) {
            return false;
        }
    }

    (void)report_daily_metrics_from_str_record(record, len, out.metrics);
    read_settings(record, len, out.settings);
    out.source_identity = record_identity(record, len);
    return true;
}

}  // namespace aircannect
