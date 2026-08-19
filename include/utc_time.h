#pragma once

#include <stdint.h>

namespace aircannect {

bool parse_utc_iso8601_ms(const char *text, int64_t &epoch_ms);

}  // namespace aircannect
