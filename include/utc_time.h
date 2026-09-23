#pragma once

#include <stdint.h>
#include <stddef.h>

namespace aircannect {

bool parse_utc_iso8601_ms(const char *text, int64_t &epoch_ms);
bool format_utc_iso8601_ms(int64_t epoch_ms, char *out, size_t size);

}  // namespace aircannect
