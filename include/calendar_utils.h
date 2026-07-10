#pragma once

#include <stdint.h>

namespace aircannect {

bool calendar_is_leap_year(int year);
uint8_t calendar_days_in_month(int year, int month);
bool calendar_parse_yyyymmdd(const char *text,
                             int &year,
                             unsigned &month,
                             unsigned &day);
bool calendar_yyyymmdd_to_days(const char *text, int64_t &days);
int64_t calendar_days_from_civil(int year, unsigned month, unsigned day);
bool calendar_civil_from_days(int64_t days,
                              int &year,
                              unsigned &month,
                              unsigned &day);

}  // namespace aircannect
