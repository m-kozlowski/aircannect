#include "display_config.h"

#include <string.h>

namespace aircannect {

const char *display_orientation_name(DisplayOrientation orientation) {
    switch (orientation) {
        case DisplayOrientation::BoardDefault: return "default";
        case DisplayOrientation::Deg0: return "0";
        case DisplayOrientation::Deg90: return "90";
        case DisplayOrientation::Deg180: return "180";
        case DisplayOrientation::Deg270: return "270";
    }
    return "default";
}

bool parse_display_orientation(const char *value,
                               DisplayOrientation &orientation) {
    if (!value) return false;
    if (strcmp(value, "default") == 0) {
        orientation = DisplayOrientation::BoardDefault;
        return true;
    }
    if (strcmp(value, "0") == 0) {
        orientation = DisplayOrientation::Deg0;
        return true;
    }
    if (strcmp(value, "90") == 0) {
        orientation = DisplayOrientation::Deg90;
        return true;
    }
    if (strcmp(value, "180") == 0) {
        orientation = DisplayOrientation::Deg180;
        return true;
    }
    if (strcmp(value, "270") == 0) {
        orientation = DisplayOrientation::Deg270;
        return true;
    }
    return false;
}

uint8_t display_orientation_rotation(DisplayOrientation orientation,
                                     uint8_t board_default) {
    switch (orientation) {
        case DisplayOrientation::BoardDefault:
            return board_default & 0x03;
        case DisplayOrientation::Deg0:
            return 0;
        case DisplayOrientation::Deg90:
            return 1;
        case DisplayOrientation::Deg180:
            return 2;
        case DisplayOrientation::Deg270:
            return 3;
    }
    return board_default & 0x03;
}

}  // namespace aircannect
