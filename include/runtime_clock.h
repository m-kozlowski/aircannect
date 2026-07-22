#pragma once

#include <stdint.h>

namespace aircannect {

inline uint32_t nonzero_millis(uint32_t now_ms) {
    return now_ms == 0 ? 1 : now_ms;
}

inline bool millis_deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return deadline_ms != 0 &&
           static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

inline bool millis_elapsed_at_least(uint32_t now_ms,
                                    uint32_t started_ms,
                                    uint32_t interval_ms) {
    return interval_ms != 0 &&
           static_cast<int32_t>(now_ms - started_ms) >=
               static_cast<int32_t>(interval_ms);
}

}  // namespace aircannect
