#pragma once

#include <stdint.h>

namespace aircannect {

bool calendar_is_leap_year(int year);
uint8_t calendar_days_in_month(int year, int month);
int64_t calendar_days_from_civil(int year, unsigned month, unsigned day);

}  // namespace aircannect
