#pragma once

#include <stdint.h>

namespace aircannect {

static constexpr int64_t REPORT_RANGE_TILE_MS = 15LL * 60LL * 1000LL;

bool normalize_report_range_tiles(int64_t from_ms,
                                  int64_t to_ms,
                                  int64_t tile_ms,
                                  int64_t max_window_ms,
                                  int64_t &normalized_from_ms,
                                  int64_t &normalized_to_ms);

}  // namespace aircannect
