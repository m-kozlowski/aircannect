#include "utc_time.h"

#include <stdio.h>
#include <time.h>

#include "calendar_utils.h"

namespace aircannect {
namespace {

static constexpr int64_t VALID_TIME_MIN_EPOCH = 1609459200;

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

    const int64_t days = calendar_days_from_civil(
        year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const int64_t seconds = days * 86400 +
                            static_cast<int64_t>(hour) * 3600 +
                            static_cast<int64_t>(minute) * 60 + second;
    if (seconds < VALID_TIME_MIN_EPOCH) return false;

    epoch_ms = seconds * 1000 + millisecond;
    return true;
}

}  // namespace

bool parse_utc_iso8601_ms(const char *text, int64_t &epoch_ms) {
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
    const char *cursor = text + consumed;
    if (*cursor == '.') {
        ++cursor;
        int digits = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            if (digits < 3) {
                millisecond = millisecond * 10 + (*cursor - '0');
            }
            ++digits;
            ++cursor;
        }
        if (digits == 0) return false;

        while (digits < 3) {
            millisecond *= 10;
            ++digits;
        }
    }

    if (*cursor != 'Z' || cursor[1] != '\0') return false;

    return utc_fields_to_epoch_ms(year, month, day, hour, minute, second,
                                  millisecond, epoch_ms);
}

bool format_utc_iso8601_ms(int64_t epoch_ms, char *out, size_t size) {
    if (!out || size == 0) return false;
    out[0] = 0;
    if (epoch_ms < VALID_TIME_MIN_EPOCH * 1000) return false;

    struct tm utc = {};
    const time_t epoch = static_cast<time_t>(epoch_ms / 1000);
    if (!gmtime_r(&epoch, &utc)) return false;

    char base[25];
    if (strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
        return false;
    }

    const int written = snprintf(out, size, "%s.%03dZ", base,
                                  static_cast<int>(epoch_ms % 1000));
    if (written < 0 || static_cast<size_t>(written) >= size) {
        out[0] = 0;
        return false;
    }
    return true;
}

}  // namespace aircannect
