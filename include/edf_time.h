#pragma once

#include <stdint.h>

namespace aircannect {

bool edf_parse_utc_ms(const char *text, int64_t &epoch_ms);

}  // namespace aircannect
