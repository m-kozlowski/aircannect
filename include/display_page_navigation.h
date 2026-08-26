#pragma once

#include <stdint.h>

namespace aircannect {

struct DisplayPageNavigation {
    uint8_t page = 0;
    bool handled = false;
    bool changed = false;
    bool wake_only = false;
};

DisplayPageNavigation display_page_navigate(uint8_t current,
                                            uint8_t page_count,
                                            int8_t direction,
                                            bool display_visible);

}  // namespace aircannect
