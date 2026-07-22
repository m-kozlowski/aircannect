#include "night_catalog_clock.h"

#include <stdlib.h>
#include <time.h>

#include "calendar_utils.h"

namespace aircannect {
namespace {

constexpr int64_t MS_PER_MINUTE = 60LL * 1000LL;
constexpr int64_t MS_PER_DAY = 24LL * 60LL * MS_PER_MINUTE;
constexpr int64_t LOCAL_NOON_MS = 12LL * 60LL * MS_PER_MINUTE;
constexpr int32_t MAX_OFFSET_MINUTES = 24 * 60;

bool valid_offset(int64_t offset_minutes) {
    return offset_minutes >= -MAX_OFFSET_MINUTES &&
           offset_minutes <= MAX_OFFSET_MINUTES;
}

bool local_noon_ms(SleepDayId sleep_day, int64_t &out) {
    if (!sleep_day.valid()) return false;

    const int64_t days = sleep_day.epoch_days();
    if (days > (INT64_MAX - LOCAL_NOON_MS) / MS_PER_DAY ||
        days < (INT64_MIN + LOCAL_NOON_MS) / MS_PER_DAY) {
        return false;
    }

    out = days * MS_PER_DAY + LOCAL_NOON_MS;
    return true;
}

bool summary_offset_minutes(const NightCatalogSummaryInput &summary,
                            int64_t local_noon,
                            int32_t &out) {
    if (summary.day_start_ms <= 0) return false;

    const int64_t delta_ms = local_noon - summary.day_start_ms;
    if ((delta_ms % MS_PER_MINUTE) != 0) return false;

    const int64_t candidate = delta_ms / MS_PER_MINUTE;
    if (!valid_offset(candidate)) return false;

    out = static_cast<int32_t>(candidate);
    return true;
}

bool posix_offset_minutes(int64_t local_ms, int32_t &out) {
    const char *timezone = getenv("TZ");
    if (!timezone || !timezone[0] || local_ms <= 0) return false;

    const int64_t local_seconds = local_ms / 1000LL;
    const int64_t days = local_seconds / (24LL * 60LL * 60LL);
    const int64_t seconds_of_day =
        local_seconds % (24LL * 60LL * 60LL);

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_civil_from_days(days, year, month, day)) return false;

    struct tm local = {};
    local.tm_year = year - 1900;
    local.tm_mon = static_cast<int>(month) - 1;
    local.tm_mday = static_cast<int>(day);
    local.tm_hour = static_cast<int>(seconds_of_day / (60LL * 60LL));
    local.tm_min = static_cast<int>((seconds_of_day / 60LL) % 60LL);
    local.tm_sec = static_cast<int>(seconds_of_day % 60LL);
    local.tm_isdst = -1;

    const time_t utc_seconds = mktime(&local);
    if (utc_seconds == static_cast<time_t>(-1)) return false;

    const int64_t offset_seconds =
        local_seconds - static_cast<int64_t>(utc_seconds);
    if ((offset_seconds % 60LL) != 0) return false;

    const int64_t candidate = offset_seconds / 60LL;
    if (!valid_offset(candidate)) return false;

    out = static_cast<int32_t>(candidate);
    return true;
}

bool resolved_offset_minutes(const NightCatalogClockContext &context,
                             SleepDayId sleep_day,
                             int64_t local_ms,
                             int32_t &out) {
    int64_t noon_ms = 0;
    if (!local_noon_ms(sleep_day, noon_ms)) return false;

    const NightCatalogSummaryInput *summary =
        night_catalog_find_summary(context, sleep_day);
    int32_t summary_offset = 0;
    if (summary &&
        summary_offset_minutes(*summary, noon_ms, summary_offset)) {
        int32_t noon_posix_offset = 0;
        int32_t local_posix_offset = 0;
        if (posix_offset_minutes(noon_ms, noon_posix_offset) &&
            posix_offset_minutes(local_ms, local_posix_offset)) {
            const int64_t adjusted =
                static_cast<int64_t>(summary_offset) +
                local_posix_offset - noon_posix_offset;
            if (!valid_offset(adjusted)) return false;
            out = static_cast<int32_t>(adjusted);
            return true;
        }

        out = summary_offset;
        return true;
    }

    if (posix_offset_minutes(local_ms, out)) return true;
    if (!context.current_offset_valid ||
        !valid_offset(context.current_offset_minutes)) {
        return false;
    }

    out = context.current_offset_minutes;
    return true;
}

}  // namespace

const NightCatalogSummaryInput *night_catalog_find_summary(
    const NightCatalogClockContext &context,
    SleepDayId sleep_day) {
    if (!context.summary_records || !sleep_day.valid()) return nullptr;

    for (size_t i = 0; i < context.summary_record_count; ++i) {
        if (context.summary_records[i].sleep_day == sleep_day) {
            return &context.summary_records[i];
        }
    }
    return nullptr;
}

bool night_catalog_resolve_local_time(
    const NightCatalogClockContext &context,
    SleepDayId sleep_day,
    int64_t local_ms,
    int64_t &utc_ms) {
    int32_t offset_minutes = 0;
    if (local_ms <= 0 ||
        !resolved_offset_minutes(context,
                                 sleep_day,
                                 local_ms,
                                 offset_minutes)) {
        return false;
    }

    const int64_t offset_ms =
        static_cast<int64_t>(offset_minutes) * MS_PER_MINUTE;
    if (offset_ms > 0 && local_ms < INT64_MIN + offset_ms) return false;
    if (offset_ms < 0 && local_ms > INT64_MAX + offset_ms) return false;

    utc_ms = local_ms - offset_ms;
    return true;
}

bool night_catalog_resolve_local_minute(void *context,
                                        SleepDayId sleep_day,
                                        uint16_t minute_from_noon,
                                        int64_t &utc_ms) {
    const NightCatalogClockContext *clock =
        static_cast<const NightCatalogClockContext *>(context);
    if (!clock || minute_from_noon > 1440) return false;

    int64_t noon_ms = 0;
    if (!local_noon_ms(sleep_day, noon_ms)) return false;

    const int64_t local_ms =
        noon_ms + static_cast<int64_t>(minute_from_noon) * MS_PER_MINUTE;
    return night_catalog_resolve_local_time(*clock,
                                            sleep_day,
                                            local_ms,
                                            utc_ms);
}

}  // namespace aircannect
