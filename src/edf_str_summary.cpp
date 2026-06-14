#include "edf_str_summary.h"

#include <time.h>

#include "edf_storage_catalog.h"
#include "report_proto.h"

namespace aircannect {

bool edf_str_summary_sleep_day(const ReportSummaryRecord &record,
                               uint16_t &day) {
    day = 0;
    if (!record.start_ms || !record.has_tz_offset_min) return false;
    const int64_t local_ms =
        static_cast<int64_t>(record.start_ms) +
        static_cast<int64_t>(record.tz_offset_min) * 60LL * 1000LL;
    if (local_ms < 0) return false;

    const time_t seconds = static_cast<time_t>(local_ms / 1000LL);
    struct tm tmv;
    if (!gmtime_r(&seconds, &tmv)) return false;

    EdfLocalDateTime local;
    local.year = tmv.tm_year + 1900;
    local.month = tmv.tm_mon + 1;
    local.day = tmv.tm_mday;
    local.hour = tmv.tm_hour;
    local.minute = tmv.tm_min;
    local.second = tmv.tm_sec;
    return edf_sleep_day_epoch_days(local, day);
}

bool edf_str_apply_summary_record(const ReportSummaryRecord &record,
                                  EdfStrSessionAccumulator &session,
                                  EdfStrSummaryApplyResult &result) {
    result = {};
    if (!session.active()) return false;
    if (!edf_str_summary_sleep_day(record, result.day) ||
        result.day != session.day_epoch_days()) {
        return false;
    }

    for (size_t i = AC_REPORT_SUMMARY_STR_FIRST_SIGNAL;
         i <= AC_REPORT_SUMMARY_STR_LAST_SIGNAL;
         ++i) {
        int16_t digital = 0;
        if (report_summary_str_sample(record, i, digital) &&
            session.set_signal_digital(i, digital)) {
            result.values++;
        }
    }
    return result.values > 0;
}

}  // namespace aircannect
