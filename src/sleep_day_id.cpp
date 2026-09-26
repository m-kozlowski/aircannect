#include "sleep_day_id.h"

#include <limits.h>
#include <stdio.h>

#include "calendar_utils.h"

namespace aircannect {
namespace {

// Valid int32 day and offset values fit in int64 millisecond arithmetic.
constexpr int64_t MS_PER_MINUTE = 60LL * 1000LL;
constexpr int64_t MS_PER_DAY = 24LL * 60LL * MS_PER_MINUTE;
constexpr int64_t LOCAL_NOON_MS = 12LL * 60LL * MS_PER_MINUTE;

}  // namespace

bool SleepDayId::from_yyyymmdd(const char *text, SleepDayId &out) {
    int64_t epoch_days = 0;
    if (!calendar_yyyymmdd_to_days(text, epoch_days)) return false;

    return from_epoch_days(epoch_days, out);
}

bool SleepDayId::from_epoch_days(int64_t epoch_days, SleepDayId &out) {
    if (epoch_days < INT32_MIN + 1LL || epoch_days > INT32_MAX) {
        return false;
    }

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_civil_from_days(epoch_days, year, month, day) ||
        year < 1 || year > 9999) {
        return false;
    }

    out = SleepDayId(static_cast<int32_t>(epoch_days));
    return true;
}

bool SleepDayId::local_noon_epoch_ms(int64_t &out) const {
    if (!valid()) return false;

    out = static_cast<int64_t>(epoch_day_) * MS_PER_DAY + LOCAL_NOON_MS;
    return true;
}

bool SleepDayId::local_minute_from_noon_epoch_ms(
    uint16_t minute_from_noon,
    int64_t &out) const {
    if (minute_from_noon > 1440) return false;

    int64_t noon_ms = 0;
    if (!local_noon_epoch_ms(noon_ms)) return false;

    const int64_t minute_offset_ms =
        static_cast<int64_t>(minute_from_noon) * MS_PER_MINUTE;
    out = noon_ms + minute_offset_ms;
    return true;
}

bool SleepDayId::utc_day_window(int32_t timezone_offset_minutes,
                                int64_t &start_ms,
                                int64_t &end_ms) const {
    int64_t local_noon_ms = 0;
    if (!local_noon_epoch_ms(local_noon_ms)) return false;

    const int64_t offset_ms =
        static_cast<int64_t>(timezone_offset_minutes) * MS_PER_MINUTE;
    const int64_t start = local_noon_ms - offset_ms;
    start_ms = start;
    end_ms = start + MS_PER_DAY;
    return true;
}

bool SleepDayId::format_yyyymmdd(char *out, size_t out_size) const {
    if (!out || out_size < 9 || !valid()) return false;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_civil_from_days(epoch_day_, year, month, day) ||
        year < 1 || year > 9999) {
        return false;
    }

    return snprintf(out, out_size, "%04d%02u%02u", year, month, day) == 8;
}

}  // namespace aircannect
