#pragma once

#include <stdint.h>

namespace aircannect {

bool edf_parse_utc_ms(const char *text, int64_t &epoch_ms);
int64_t edf_floor_epoch_ms_to_second(int64_t epoch_ms);

}  // namespace aircannect
