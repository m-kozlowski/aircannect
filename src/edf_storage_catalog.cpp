#include "edf_storage_catalog.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {
namespace {

bool leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
    static const int days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month == 2 && leap_year(year)) return 29;
    if (month < 1 || month > 12) return 0;
    return days[month - 1];
}

bool valid_date_time(const EdfLocalDateTime &dt) {
    return dt.year >= 2020 &&
           dt.month >= 1 && dt.month <= 12 &&
           dt.day >= 1 && dt.day <= days_in_month(dt.year, dt.month) &&
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
        dt.day = days_in_month(dt.year, dt.month);
        return dt;
    }
    dt.year--;
    dt.month = 12;
    dt.day = 31;
    return dt;
}

int64_t days_before_year(int year) {
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) {
        days += leap_year(y) ? 366 : 365;
    }
    return days;
}

int64_t epoch_days(const EdfLocalDateTime &dt) {
    int64_t days = days_before_year(dt.year);
    for (int month = 1; month < dt.month; ++month) {
        days += days_in_month(dt.year, month);
    }
    return days + dt.day - 1;
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
    const int64_t parsed = epoch_days(sleep_day);
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

bool edf_valid_browse_path(const char *path) {
    if (!path) return false;
    const size_t len = strlen(path);
    if (len == 0 || len >= AC_STORAGE_WRITE_PATH_MAX) return false;
    if (strcmp(path, "/") == 0 || strcmp(path, "/DATALOG") == 0) {
        return true;
    }

    static constexpr char kPrefix[] = "/DATALOG/";
    static constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (len != kPrefixLen + 8 ||
        strncmp(path, kPrefix, kPrefixLen) != 0) {
        return false;
    }

    EdfLocalDateTime day;
    return parse_yyyymmdd(path + kPrefixLen, day);
}

bool edf_valid_pull_path(const char *path) {
    if (!path) return false;
    const size_t len = strlen(path);
    if (len == 0 || len >= AC_STORAGE_WRITE_PATH_MAX) return false;
    if (strcmp(path, "/STR.edf") == 0) return true;

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
