#pragma once

#include <stdint.h>

namespace aircannect {

enum class DisplayOrientation : uint8_t {
    BoardDefault,
    Deg0,
    Deg90,
    Deg180,
    Deg270,
};

const char *display_orientation_name(DisplayOrientation orientation);
bool parse_display_orientation(const char *value,
                               DisplayOrientation &orientation);
uint8_t display_orientation_rotation(DisplayOrientation orientation,
                                     uint8_t board_default);

}  // namespace aircannect
