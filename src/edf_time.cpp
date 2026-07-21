#include "edf_time.h"

#include <stdio.h>
#include <time.h>

#include "calendar_utils.h"

namespace aircannect {
namespace {

static constexpr time_t VALID_TIME_MIN_EPOCH = 1609459200;  // 2021-01-01

bool utc_fields_to_epoch_ms(int year,
                            int month,
                            int day,
                            int hour,
                            int minute,
                            int second,
                            int millisecond,
                            int64_t &epoch_ms) {
    if (year < 2020 || month < 1 || month > 12 || day < 1 ||
        day > calendar_days_in_month(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 ||
        millisecond < 0 || millisecond > 999) {
        return false;
    }
    const int64_t days =
        calendar_days_from_civil(year, static_cast<unsigned>(month),
                                 static_cast<unsigned>(day));
    const int64_t seconds = days * 86400 +
                            static_cast<int64_t>(hour) * 3600 +
                            static_cast<int64_t>(minute) * 60 + second;
    if (seconds < static_cast<int64_t>(VALID_TIME_MIN_EPOCH)) return false;
    epoch_ms = seconds * 1000 + millisecond;
    return true;
}

}  // namespace

bool edf_parse_utc_ms(const char *text, int64_t &epoch_ms) {
    if (!text || !*text) return false;

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n",
               &year, &month, &day, &hour, &minute, &second,
               &consumed) != 6) {
        return false;
    }

    int millisecond = 0;
    const char *p = text + consumed;
    if (*p == '.') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) {
                millisecond = millisecond * 10 + (*p - '0');
            }
            digits++;
            p++;
        }
        if (digits == 0) return false;
        while (digits < 3) {
            millisecond *= 10;
            digits++;
        }
    }

    if (*p != 'Z' || p[1] != 0) return false;
    return utc_fields_to_epoch_ms(year, month, day, hour, minute, second,
                                  millisecond, epoch_ms);
}

bool edf_parse_as11_utc_ms(const char *text,
                           const As11ClockTransform &transform,
                           int64_t &epoch_ms) {
    int64_t device_epoch_ms = 0;
    if (!edf_parse_utc_ms(text, device_epoch_ms)) return false;
    return transform.to_utc_ms(device_epoch_ms, epoch_ms);
}

int64_t edf_floor_epoch_ms_to_second(int64_t epoch_ms) {
    const int64_t rem = epoch_ms % 1000;
    if (rem < 0) return epoch_ms - rem - 1000;
    return epoch_ms - rem;
}

}  // namespace aircannect
