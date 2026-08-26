#include "board_button.h"

#include <string.h>

namespace aircannect {

const BoardButtonDefinition *board_button_catalog(size_t &count) {
#if AC_BUTTON_PROFILE == AC_BUTTON_PROFILE_WAVESHARE_154
    static constexpr BoardButtonDefinition BUTTONS[] = {
        {1, "boot", "BOOT", 0, true, BoardButtonPull::Up,
         BUTTON_GESTURE_SHORT | BUTTON_GESTURE_LONG, 35, 1200,
         LocalActionId::DisplayPreviousPage, LocalActionId::None},
        {2, "power", "PWR", 5, true, BoardButtonPull::Up,
         BUTTON_GESTURE_SHORT | BUTTON_GESTURE_LONG, 35, 1200,
         LocalActionId::DisplayToggleBacklight,
         LocalActionId::TherapyToggle},
        {3, "plus", "PLUS", 4, true, BoardButtonPull::Up,
         BUTTON_GESTURE_SHORT | BUTTON_GESTURE_LONG, 35, 1200,
         LocalActionId::DisplayNextPage, LocalActionId::None},
    };
    count = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
    return BUTTONS;
#else
    count = 0;
    return nullptr;
#endif
}

const BoardButtonDefinition *board_button_find(uint16_t key) {
    size_t count = 0;
    const BoardButtonDefinition *buttons = board_button_catalog(count);
    for (size_t i = 0; i < count; ++i) {
        if (buttons[i].key == key) return &buttons[i];
    }
    return nullptr;
}

const BoardButtonDefinition *board_button_find(const char *id) {
    if (!id) return nullptr;

    size_t count = 0;
    const BoardButtonDefinition *buttons = board_button_catalog(count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(buttons[i].id, id) == 0) return &buttons[i];
    }
    return nullptr;
}

}  // namespace aircannect
