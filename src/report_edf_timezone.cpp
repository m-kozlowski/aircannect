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
constexpr int64_t SECONDS_PER_HOUR = 60LL * 60LL;
constexpr int32_t MAX_TIMEZONE_OFFSET_MINUTES = 24 * 60;

bool sleep_day_local_noon_seconds(const char *sleep_day,
                                  int64_t &local_seconds) {
    if (!sleep_day || strlen(sleep_day) != 8) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (sleep_day[i] < '0' || sleep_day[i] > '9') return false;
    }

    char value[5] = {};
    memcpy(value, sleep_day, 4);
    const int year = static_cast<int>(strtol(value, nullptr, 10));

    value[0] = sleep_day[4];
    value[1] = sleep_day[5];
    value[2] = '\0';
    const unsigned month = static_cast<unsigned>(strtoul(value, nullptr, 10));

    value[0] = sleep_day[6];
    value[1] = sleep_day[7];
    const unsigned day = static_cast<unsigned>(strtoul(value, nullptr, 10));

    if (year <= 0 || month < 1 || month > 12 || day < 1 ||
        day > calendar_days_in_month(year, static_cast<int>(month))) {
        return false;
    }

    local_seconds = calendar_days_from_civil(year, month, day) *
                    SECONDS_PER_DAY + 12LL * SECONDS_PER_HOUR;
    return true;
}

bool summary_period_offset_minutes(const ReportSummaryRecord &summary,
                                   const char *sleep_day,
                                   int32_t &offset_minutes) {
    if (!summary.valid || !summary.start_ms ||
        !report_edf_summary_matches_sleep_day(summary, sleep_day)) {
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

    int32_t summary_offset_minutes = 0;
    if (matching_summary &&
        summary_period_offset_minutes(*matching_summary,
                                      session.sleep_day,
                                      summary_offset_minutes)) {
        return apply_resolution(session,
                                summary_offset_minutes,
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
