#include "edf_storage_catalog.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "calendar_utils.h"
#include "edf_identification.h"

namespace aircannect {
namespace {

bool valid_date_time(const EdfLocalDateTime &dt) {
    return dt.year >= 2020 &&
           dt.month >= 1 && dt.month <= 12 &&
           dt.day >= 1 &&
           dt.day <= calendar_days_in_month(dt.year, dt.month) &&
           dt.hour >= 0 && dt.hour <= 23 &&
           dt.minute >= 0 && dt.minute <= 59 &&
           dt.second >= 0 && dt.second <= 59;
}

EdfLocalDateTime previous_day(EdfLocalDateTime dt) {
    dt.hour = 0;
    dt.minute = 0;
    dt.second = 0;
    dt.day--;
    if (dt.day >= 1) return dt;
    dt.month--;
    if (dt.month >= 1) {
        dt.day = calendar_days_in_month(dt.year, dt.month);
        return dt;
    }
    dt.year--;
    dt.month = 12;
    dt.day = 31;
    return dt;
}

bool date_from_epoch_days(int64_t days, EdfLocalDateTime &dt) {
    if (days < 0) return false;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_civil_from_days(days, year, month, day) ||
        year > 9999) {
        return false;
    }

    dt.year = year;
    dt.month = static_cast<int>(month);
    dt.day = static_cast<int>(day);
    return true;
}

bool write_yyyymmdd(const EdfLocalDateTime &dt,
                    char *dst,
                    size_t dst_size) {
    if (!dst || dst_size < 9 || !valid_date_time(dt)) return false;
    const int written = snprintf(dst, dst_size, "%04d%02d%02d",
                                 dt.year, dt.month, dt.day);
    return written == 8;
}

const char *month_name(int month) {
    static const char *const names[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
    };
    if (month < 1 || month > 12) return nullptr;
    return names[month - 1];
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool parse_digits(const char *text, size_t len, int &out) {
    if (!text || len == 0) return false;
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!is_digit(text[i])) return false;
        value = value * 10 + (text[i] - '0');
    }
    out = value;
    return true;
}

bool parse_yyyymmdd(const char *text, EdfLocalDateTime &out) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (!parse_digits(text, 4, year) ||
        !parse_digits(text + 4, 2, month) ||
        !parse_digits(text + 6, 2, day)) {
        return false;
    }
    EdfLocalDateTime dt;
    dt.year = year;
    dt.month = month;
    dt.day = day;
    dt.hour = 0;
    dt.minute = 0;
    dt.second = 0;
    if (!valid_date_time(dt)) return false;
    out = dt;
    return true;
}

bool parse_session_stamp(const char *text, EdfLocalDateTime &out) {
    int hour = 0;
    int minute = 0;
    int second = 0;
    EdfLocalDateTime dt;
    if (!parse_yyyymmdd(text, dt) ||
        text[8] != '_' ||
        !parse_digits(text + 9, 2, hour) ||
        !parse_digits(text + 11, 2, minute) ||
        !parse_digits(text + 13, 2, second)) {
        return false;
    }
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    if (!valid_date_time(dt)) return false;
    out = dt;
    return true;
}

bool pull_tag_allowed(const char *tag) {
    return tag &&
           (strncmp(tag, "BRP", 3) == 0 ||
            strncmp(tag, "PLD", 3) == 0 ||
            strncmp(tag, "SA2", 3) == 0 ||
            strncmp(tag, "EVE", 3) == 0 ||
            strncmp(tag, "CSL", 3) == 0);
}

}  // namespace

const char *edf_file_tag(EdfFileKind kind) {
    switch (kind) {
        case EdfFileKind::Brp: return "BRP";
        case EdfFileKind::Pld: return "PLD";
        case EdfFileKind::Sa2: return "SA2";
        default: return "EDF";
    }
}

const char *edf_annotation_file_tag(EdfAnnotationKind kind) {
    switch (kind) {
        case EdfAnnotationKind::Eve: return "EVE";
        case EdfAnnotationKind::Csl: return "CSL";
        default: return "EDF";
    }
}

bool edf_parse_as11_local_datetime(const char *text,
                                   EdfLocalDateTime &out) {
    if (!text || !*text) return false;
    EdfLocalDateTime dt;
    int consumed = 0;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n",
               &dt.year, &dt.month, &dt.day,
               &dt.hour, &dt.minute, &dt.second, &consumed) != 6) {
        return false;
    }
    const char *p = text + consumed;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == 'Z') p++;
    if (*p != 0) return false;
    if (!valid_date_time(dt)) return false;
    out = dt;
    return true;
}

bool edf_epoch_ms_to_local_datetime(int64_t epoch_ms,
                                    int32_t timezone_offset_minutes,
                                    EdfLocalDateTime &out) {
    static constexpr int32_t kMaxTimezoneOffsetMinutes = 24 * 60;
    static constexpr int64_t kMsPerSecond = 1000;
    static constexpr int64_t kMsPerMinute = 60 * kMsPerSecond;
    static constexpr int64_t kSecondsPerDay = 86400;

    if (timezone_offset_minutes < -kMaxTimezoneOffsetMinutes ||
        timezone_offset_minutes > kMaxTimezoneOffsetMinutes) {
        return false;
    }

    const int64_t offset_ms =
        static_cast<int64_t>(timezone_offset_minutes) * kMsPerMinute;
    if ((offset_ms > 0 && epoch_ms > INT64_MAX - offset_ms) ||
        (offset_ms < 0 && epoch_ms < INT64_MIN - offset_ms)) {
        return false;
    }

    const int64_t local_ms = epoch_ms + offset_ms;
    int64_t local_seconds = local_ms / kMsPerSecond;
    if (local_ms < 0 && local_ms % kMsPerSecond) local_seconds--;

    int64_t days = local_seconds / kSecondsPerDay;
    int64_t seconds_of_day = local_seconds % kSecondsPerDay;
    if (seconds_of_day < 0) {
        seconds_of_day += kSecondsPerDay;
        days--;
    }

    EdfLocalDateTime dt;
    if (!date_from_epoch_days(days, dt)) return false;
    dt.hour = static_cast<int>(seconds_of_day / 3600);
    seconds_of_day %= 3600;
    dt.minute = static_cast<int>(seconds_of_day / 60);
    dt.second = static_cast<int>(seconds_of_day % 60);
    if (!valid_date_time(dt)) return false;

    out = dt;
    return true;
}

bool edf_epoch_ms_to_configured_local_datetime(int64_t epoch_ms,
                                               EdfLocalDateTime &out) {
    static constexpr int64_t kMsPerSecond = 1000;
    if (epoch_ms < 0) return false;

    const int64_t seconds64 = epoch_ms / kMsPerSecond;
    const time_t seconds = static_cast<time_t>(seconds64);
    if (static_cast<int64_t>(seconds) != seconds64) return false;

    struct tm local = {};
    if (!localtime_r(&seconds, &local)) return false;

    EdfLocalDateTime dt;
    dt.year = local.tm_year + 1900;
    dt.month = local.tm_mon + 1;
    dt.day = local.tm_mday;
    dt.hour = local.tm_hour;
    dt.minute = local.tm_min;
    dt.second = local.tm_sec;
    if (!valid_date_time(dt)) return false;
    out = dt;
    return true;
}

bool edf_sleep_day_yyyymmdd(const EdfLocalDateTime &dt,
                            char *dst,
                            size_t dst_size) {
    if (!valid_date_time(dt)) return false;
    const EdfLocalDateTime sleep_day =
        dt.hour >= 12 ? dt : previous_day(dt);
    return write_yyyymmdd(sleep_day, dst, dst_size);
}

bool edf_session_stamp(const EdfLocalDateTime &dt,
                       char *dst,
                       size_t dst_size) {
    if (!dst || dst_size < 16 || !valid_date_time(dt)) return false;
    const int written = snprintf(dst, dst_size, "%04d%02d%02d_%02d%02d%02d",
                                 dt.year, dt.month, dt.day,
                                 dt.hour, dt.minute, dt.second);
    return written == 15;
}

bool edf_header_date(const EdfLocalDateTime &dt,
                     char *dst,
                     size_t dst_size) {
    if (!dst || dst_size < 9 || !valid_date_time(dt)) return false;
    const int written = snprintf(dst, dst_size, "%02d.%02d.%02d",
                                 dt.day, dt.month, dt.year % 100);
    return written == 8;
}

bool edf_header_time(const EdfLocalDateTime &dt,
                     char *dst,
                     size_t dst_size) {
    if (!dst || dst_size < 9 || !valid_date_time(dt)) return false;
    const int written = snprintf(dst, dst_size, "%02d.%02d.%02d",
                                 dt.hour, dt.minute, dt.second);
    return written == 8;
}

bool edf_recording_id(const EdfLocalDateTime &dt,
                      const char *serial_number,
                      int32_t platform_id,
                      int32_t variant_id,
                      char *dst,
                      size_t dst_size) {
    if (!dst || dst_size == 0 || !serial_number || !serial_number[0] ||
        !valid_date_time(dt) || platform_id < 0 || variant_id < 0) {
        return false;
    }
    const char *month = month_name(dt.month);
    if (!month) return false;
    const int written = snprintf(dst, dst_size,
                                 "Startdate %02d-%s-%04d X X X SRN=%s "
                                 "MID=%ld VID=%ld",
                                 dt.day,
                                 month,
                                 dt.year,
                                 serial_number,
                                 static_cast<long>(platform_id),
                                 static_cast<long>(variant_id));
    return written > 0 && static_cast<size_t>(written) < dst_size &&
           written <= 80;
}

bool edf_sleep_day_start(const EdfLocalDateTime &dt,
                         EdfLocalDateTime &start) {
    if (!valid_date_time(dt)) return false;
    start = dt.hour >= 12 ? dt : previous_day(dt);
    start.hour = 12;
    start.minute = 0;
    start.second = 0;
    return true;
}

bool edf_sleep_day_epoch_days(const EdfLocalDateTime &dt,
                              uint16_t &days) {
    if (!valid_date_time(dt)) return false;
    EdfLocalDateTime sleep_day;
    if (!edf_sleep_day_start(dt, sleep_day)) return false;
    const int64_t parsed =
        calendar_days_from_civil(sleep_day.year,
                                 static_cast<unsigned>(sleep_day.month),
                                 static_cast<unsigned>(sleep_day.day));
    if (parsed < 0 || parsed > 24836) return false;
    days = static_cast<uint16_t>(parsed);
    return true;
}

bool edf_sleep_day_minute(const EdfLocalDateTime &dt,
                          uint16_t &minute) {
    if (!valid_date_time(dt)) return false;
    const int parsed =
        dt.hour >= 12
            ? (dt.hour - 12) * 60 + dt.minute
            : (dt.hour + 12) * 60 + dt.minute;
    if (parsed < 0 || parsed > 1440) return false;
    minute = static_cast<uint16_t>(parsed);
    return true;
}

bool edf_datalog_dir(const EdfLocalDateTime &dt,
                     char *dst,
                     size_t dst_size) {
    char day[9] = {};
    if (!edf_sleep_day_yyyymmdd(dt, day, sizeof(day))) return false;
    if (!dst || dst_size < 18) return false;
    const int written = snprintf(dst, dst_size, "/DATALOG/%s", day);
    return written > 0 && static_cast<size_t>(written) < dst_size;
}

bool edf_datalog_path(EdfFileKind kind,
                      const EdfLocalDateTime &dt,
                      char *dst,
                      size_t dst_size) {
    char dir[18] = {};
    char stamp[16] = {};
    if (!edf_datalog_dir(dt, dir, sizeof(dir)) ||
        !edf_session_stamp(dt, stamp, sizeof(stamp))) {
        return false;
    }
    if (!dst || dst_size == 0) return false;
    const int written = snprintf(dst, dst_size, "%s/%s_%s.edf",
                                 dir, stamp, edf_file_tag(kind));
    return written > 0 && static_cast<size_t>(written) < dst_size;
}

bool edf_datalog_annotation_path(EdfAnnotationKind kind,
                                 const EdfLocalDateTime &dt,
                                 char *dst,
                                 size_t dst_size) {
    char dir[18] = {};
    char stamp[16] = {};
    if (!edf_datalog_dir(dt, dir, sizeof(dir)) ||
        !edf_session_stamp(dt, stamp, sizeof(stamp))) {
        return false;
    }
    if (!dst || dst_size == 0) return false;
    const int written = snprintf(dst, dst_size, "%s/%s_%s.edf",
                                 dir, stamp, edf_annotation_file_tag(kind));
    return written > 0 && static_cast<size_t>(written) < dst_size;
}

bool edf_str_path(char *dst, size_t dst_size) {
    if (!dst || dst_size < 9) return false;
    const int written = snprintf(dst, dst_size, "/STR.edf");
    return written == 8;
}

bool edf_valid_pull_path(const char *path) {
    if (!path) return false;
    const size_t len = strlen(path);
    if (len == 0 || len >= AC_STORAGE_WRITE_PATH_MAX) return false;
    if (strcmp(path, "/STR.edf") == 0 ||
        strcmp(path, AC_EDF_IDENTIFICATION_JSON_PATH) == 0 ||
        strcmp(path, AC_EDF_IDENTIFICATION_CRC_PATH) == 0) {
        return true;
    }

    static constexpr size_t kPathLen = 41;
    static constexpr char kPrefix[] = "/DATALOG/";
    static constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (len != kPathLen || strncmp(path, kPrefix, kPrefixLen) != 0) {
        return false;
    }
    if (path[17] != '/' || path[26] != '_' || path[33] != '_' ||
        strcmp(path + 37, ".edf") != 0) {
        return false;
    }

    EdfLocalDateTime path_day;
    EdfLocalDateTime session;
    if (!parse_yyyymmdd(path + 9, path_day) ||
        !parse_session_stamp(path + 18, session) ||
        !pull_tag_allowed(path + 34)) {
        return false;
    }

    char expected_day[9] = {};
    char actual_day[9] = {};
    if (!edf_sleep_day_yyyymmdd(session, expected_day, sizeof(expected_day)) ||
        !write_yyyymmdd(path_day, actual_day, sizeof(actual_day))) {
        return false;
    }
    return strcmp(expected_day, actual_day) == 0;
}

}  // namespace aircannect
