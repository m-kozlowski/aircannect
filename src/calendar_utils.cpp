#include "calendar_utils.h"

#include <stddef.h>

namespace aircannect {

bool calendar_is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t calendar_days_in_month(int year, int month) {
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && calendar_is_leap_year(year)) return 29;
    return days[month - 1];
}

bool calendar_parse_yyyymmdd(const char *text,
                             int &year,
                             unsigned &month,
                             unsigned &day) {
    if (!text) return false;

    unsigned values[3] = {};
    constexpr uint8_t widths[] = {4, 2, 2};
    size_t offset = 0;
    for (size_t field = 0; field < 3; ++field) {
        for (uint8_t digit = 0; digit < widths[field]; ++digit) {
            const char c = text[offset++];
            if (c < '0' || c > '9') return false;
            values[field] = values[field] * 10u +
                            static_cast<unsigned>(c - '0');
        }
    }
    if (text[offset] != '\0') return false;

    year = static_cast<int>(values[0]);
    month = values[1];
    day = values[2];
    return year > 0 && month >= 1 && month <= 12 && day >= 1 &&
           day <= calendar_days_in_month(year, static_cast<int>(month));
}

bool calendar_yyyymmdd_to_days(const char *text, int64_t &days) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!calendar_parse_yyyymmdd(text, year, month, day)) return false;

    days = calendar_days_from_civil(year, month, day);
    return true;
}

int64_t calendar_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 +
           static_cast<int64_t>(doe) - 719468;
}

bool calendar_civil_from_days(int64_t days,
                              int &year,
                              unsigned &month,
                              unsigned &day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;

    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = y + (month <= 2);

    return year >= 0 &&
           month >= 1 && month <= 12 &&
           day >= 1 &&
           day <= calendar_days_in_month(year, static_cast<int>(month));
}

}  // namespace aircannect
