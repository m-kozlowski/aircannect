#include "display_page_navigation.h"

namespace aircannect {

DisplayPageNavigation display_page_navigate(uint8_t current,
                                            uint8_t page_count,
                                            int8_t direction,
                                            bool display_visible) {
    DisplayPageNavigation out;
    out.page = current;
    if (page_count == 0 || direction == 0) return out;

    out.handled = true;
    if (!display_visible) {
        out.wake_only = true;
        return out;
    }

    current %= page_count;
    out.page = direction > 0
                   ? static_cast<uint8_t>((current + 1) % page_count)
                   : static_cast<uint8_t>(
                         (current + page_count - 1) % page_count);
    out.changed = out.page != current;
    return out;
}

}  // namespace aircannect
