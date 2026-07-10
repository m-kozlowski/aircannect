#include "report_edf_timezone.h"

#include <stdlib.h>
#include <time.h>

#include "calendar_utils.h"
#include "edf_report_session.h"
#include "report_night_index.h"

namespace aircannect {
namespace {

constexpr int64_t SECONDS_PER_DAY = 24LL * 60LL * 60LL;
constexpr int64_t SECONDS_PER_HOUR = 60LL * 60LL;
constexpr int32_t MAX_TIMEZONE_OFFSET_MINUTES = 24 * 60;

bool sleep_day_local_noon_seconds(const char *sleep_day,
                                  int64_t &local_seconds) {
    int64_t days = 0;
    if (!calendar_yyyymmdd_to_days(sleep_day, days)) {
        return false;
    }

    local_seconds = days * SECONDS_PER_DAY + 12LL * SECONDS_PER_HOUR;
    return true;
}

bool summary_period_offset_minutes(const ReportSummaryRecord &summary,
                                   const char *sleep_day,
                                   int32_t &offset_minutes) {
    if (!summary.valid || !summary.start_ms ||
        !report_summary_matches_sleep_day(summary, sleep_day)) {
        return false;
    }

    int64_t local_noon_seconds = 0;
    if (!sleep_day_local_noon_seconds(sleep_day, local_noon_seconds)) {
        return false;
    }

    const int64_t local_noon_ms = local_noon_seconds * 1000LL;
    const int64_t offset_ms =
        local_noon_ms - static_cast<int64_t>(summary.start_ms);
    if (offset_ms % 60000LL != 0) return false;

    const int64_t candidate = offset_ms / 60000LL;
    if (candidate < -MAX_TIMEZONE_OFFSET_MINUTES ||
        candidate > MAX_TIMEZONE_OFFSET_MINUTES) {
        return false;
    }

    offset_minutes = static_cast<int32_t>(candidate);
    return true;
}

bool posix_offset_for_local_ms(int64_t local_ms, int32_t &offset_minutes) {
    const char *timezone = getenv("TZ");
    if (!timezone || !timezone[0] || local_ms <= 0) return false;

    const int64_t local_seconds = local_ms / 1000LL;
    const int64_t days = local_seconds / SECONDS_PER_DAY;
    const int64_t seconds_of_day = local_seconds % SECONDS_PER_DAY;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_civil_from_days(days, year, month, day)) return false;

    struct tm local = {};
    local.tm_year = year - 1900;
    local.tm_mon = static_cast<int>(month) - 1;
    local.tm_mday = static_cast<int>(day);
    local.tm_hour = static_cast<int>(seconds_of_day / 3600LL);
    local.tm_min = static_cast<int>((seconds_of_day % 3600LL) / 60LL);
    local.tm_sec = static_cast<int>(seconds_of_day % 60LL);
    local.tm_isdst = -1;

    const time_t utc_seconds = mktime(&local);
    if (utc_seconds == static_cast<time_t>(-1)) return false;

    const int64_t offset_seconds =
        local_seconds - static_cast<int64_t>(utc_seconds);
    if (offset_seconds % 60LL != 0) return false;

    const int64_t candidate = offset_seconds / 60LL;
    if (candidate < -MAX_TIMEZONE_OFFSET_MINUTES ||
        candidate > MAX_TIMEZONE_OFFSET_MINUTES) {
        return false;
    }

    offset_minutes = static_cast<int32_t>(candidate);
    return true;
}

bool apply_resolution(EdfReportSessionDescriptor &session,
                      int32_t offset_minutes,
                      int32_t &resolved_offset_minutes) {
    const bool has_local_time =
        session.local_earliest_header_start_ms > 0;
    if (has_local_time &&
        !edf_report_session_apply_timezone_offset(session, offset_minutes)) {
        return false;
    }

    resolved_offset_minutes = offset_minutes;
    return true;
}

}  // namespace

bool report_edf_resolve_session_timezone(
    EdfReportSessionDescriptor &session,
    const ReportSummaryRecord *matching_summary,
    bool current_offset_valid,
    int32_t current_offset_minutes,
    int32_t &resolved_offset_minutes) {
    resolved_offset_minutes = 0;

    int32_t summary_offset_minutes = 0;
    if (matching_summary &&
        summary_period_offset_minutes(*matching_summary,
                                      session.sleep_day,
                                      summary_offset_minutes)) {
        return apply_resolution(session,
                                summary_offset_minutes,
                                resolved_offset_minutes);
    }

    int32_t posix_offset_minutes = 0;
    if (posix_offset_for_local_ms(session.local_earliest_header_start_ms,
                                  posix_offset_minutes)) {
        return apply_resolution(session,
                                posix_offset_minutes,
                                resolved_offset_minutes);
    }

    if (current_offset_valid) {
        return apply_resolution(session,
                                current_offset_minutes,
                                resolved_offset_minutes);
    }

    return false;
}

}  // namespace aircannect
