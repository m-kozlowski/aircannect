#include "report_range_tile.h"

#include <limits.h>

namespace aircannect {

bool normalize_report_range_tiles(int64_t from_ms,
                                  int64_t to_ms,
                                  int64_t tile_ms,
                                  int64_t max_window_ms,
                                  int64_t &normalized_from_ms,
                                  int64_t &normalized_to_ms) {
    normalized_from_ms = 0;
    normalized_to_ms = 0;
    if (from_ms < 0 || to_ms <= from_ms || tile_ms <= 0 ||
        max_window_ms <= 0 || to_ms > INT64_MAX - (tile_ms - 1)) {
        return false;
    }

    const int64_t normalized_from = (from_ms / tile_ms) * tile_ms;
    const int64_t normalized_to =
        ((to_ms + tile_ms - 1) / tile_ms) * tile_ms;
    if (normalized_to <= normalized_from ||
        normalized_to - normalized_from > max_window_ms) {
        return false;
    }

    normalized_from_ms = normalized_from;
    normalized_to_ms = normalized_to;
    return true;
}

}  // namespace aircannect
