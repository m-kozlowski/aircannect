#include "report_edf_timezone.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "calendar_utils.h"
#include "edf_report_session.h"
#include "report_night_index.h"

namespace aircannect {
namespace {

constexpr int64_t SECONDS_PER_DAY = 24LL * 60LL * 60LL;
constexpr int32_t MAX_TIMEZONE_OFFSET_MINUTES = 24 * 60;

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
                      ReportEdfTimezoneSource source,
                      ReportEdfTimezoneResolution &out) {
    const bool has_local_time =
        session.local_earliest_header_start_ms > 0;
    if (has_local_time &&
        !edf_report_session_apply_timezone_offset(session, offset_minutes)) {
        return false;
    }

    out.valid = true;
    out.offset_minutes = offset_minutes;
    out.source = source;
    return true;
}

}  // namespace

bool report_edf_summary_matches_sleep_day(
    const ReportSummaryRecord &summary,
    const char *sleep_day) {
    if (!sleep_day || !sleep_day[0]) return false;

    char summary_sleep_day[9] = {};
    return report_summary_sleep_day_yyyymmdd(summary,
                                             summary_sleep_day,
                                             sizeof(summary_sleep_day)) &&
           strcmp(summary_sleep_day, sleep_day) == 0;
}

bool report_edf_resolve_session_timezone(
    EdfReportSessionDescriptor &session,
    const ReportSummaryRecord *matching_summary,
    bool current_offset_valid,
    int32_t current_offset_minutes,
    ReportEdfTimezoneResolution &out) {
    out = {};

    if (matching_summary && matching_summary->has_tz_offset_min &&
        report_edf_summary_matches_sleep_day(*matching_summary,
                                             session.sleep_day)) {
        return apply_resolution(session,
                                matching_summary->tz_offset_min,
                                ReportEdfTimezoneSource::Summary,
                                out);
    }

    int32_t posix_offset_minutes = 0;
    if (posix_offset_for_local_ms(session.local_earliest_header_start_ms,
                                  posix_offset_minutes)) {
        return apply_resolution(session,
                                posix_offset_minutes,
                                ReportEdfTimezoneSource::PosixRule,
                                out);
    }

    if (current_offset_valid) {
        return apply_resolution(session,
                                current_offset_minutes,
                                ReportEdfTimezoneSource::CurrentDeviceOffset,
                                out);
    }

    return false;
}

}  // namespace aircannect
