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

}  // namespace aircannect
