#include "sleep_day_id.h"

#include <limits.h>
#include <stdio.h>

#include "calendar_utils.h"

namespace aircannect {

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
