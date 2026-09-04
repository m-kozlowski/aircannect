#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr int64_t REPORT_RANGE_TILE_MS = 15LL * 60LL * 1000LL;
static constexpr size_t REPORT_RANGE_TILE_BATCH_MAX = 12;

}  // namespace aircannect
