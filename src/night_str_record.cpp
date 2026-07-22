#include "night_str_record.h"

#include "edf_str_record_reader.h"

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
    out.source_identity = record_identity(record, len);
    return true;
}

}  // namespace aircannect
