#include "board_button.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {

const BoardButtonDefinition *board_button_catalog(size_t &count) {
#if AC_BUTTON_PROFILE == AC_BUTTON_PROFILE_WAVESHARE_154
    static constexpr BoardButtonDefinition BUTTONS[] = {
        {1, "boot", "BOOT", 0, true, BoardButtonPull::Up,
         BUTTON_GESTURE_SHORT | BUTTON_GESTURE_LONG, 35, 1200},
        {2, "power", "PWR", 5, true, BoardButtonPull::Up,
         BUTTON_GESTURE_SHORT | BUTTON_GESTURE_LONG, 35, 1200},
        {3, "plus", "PLUS", 4, true, BoardButtonPull::Up,
         BUTTON_GESTURE_SHORT | BUTTON_GESTURE_LONG, 35, 1200},
    };
    count = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
    return BUTTONS;
#else
    count = 0;
    return nullptr;
#endif
}

const ButtonBinding *board_button_binding_defaults(size_t &count) {
#if AC_BUTTON_PROFILE == AC_BUTTON_PROFILE_WAVESHARE_154
    static constexpr ButtonBinding DEFAULTS[] = {
        {{1, 0}, ButtonGesture::ShortPress,
         LocalActionId::DisplayPreviousPage},
        {{2, 0}, ButtonGesture::ShortPress,
         LocalActionId::DisplayToggleBacklight},
        {{2, 0}, ButtonGesture::LongPress,
         LocalActionId::TherapyToggle},
        {{3, 0}, ButtonGesture::ShortPress,
         LocalActionId::DisplayNextPage},
        {{1, 3}, ButtonGesture::LongPress,
         LocalActionId::PowerOff},
    };
    count = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
    return DEFAULTS;
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

bool board_button_input_find(const char *id, ButtonInput &input) {
    input = {};
    const BoardButtonDefinition *single = board_button_find(id);
    if (single) {
        input = button_input(single->key);
        return true;
    }
    if (!id) return false;

    const char *separator = strchr(id, '+');
    if (!separator || separator == id || !separator[1] ||
        strchr(separator + 1, '+')) {
        return false;
    }

    char first_id[24] = {};
    char second_id[24] = {};
    const size_t first_length = static_cast<size_t>(separator - id);
    const size_t second_length = strlen(separator + 1);
    if (first_length >= sizeof(first_id) ||
        second_length >= sizeof(second_id)) {
        return false;
    }

    memcpy(first_id, id, first_length);
    memcpy(second_id, separator + 1, second_length);
    const BoardButtonDefinition *first = board_button_find(first_id);
    const BoardButtonDefinition *second = board_button_find(second_id);
    if (!first || !second || first->key == second->key) return false;

    input = button_input(first->key, second->key);
    return true;
}

bool board_button_input_supported(const ButtonInput &input,
                                  ButtonGesture gesture) {
    const BoardButtonDefinition *first =
        board_button_find(input.first_button_key);
    if (!first) return false;

    if (!input.chord()) {
        return board_button_supports_gesture(*first, gesture);
    }

    return input.second_button_key != input.first_button_key &&
           board_button_find(input.second_button_key) != nullptr &&
           (gesture == ButtonGesture::ShortPress ||
            gesture == ButtonGesture::LongPress);
}

bool board_button_input_id(const ButtonInput &input,
                           char *out,
                           size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';

    const BoardButtonDefinition *first =
        board_button_find(input.first_button_key);
    const BoardButtonDefinition *second = input.chord()
        ? board_button_find(input.second_button_key)
        : nullptr;
    if (!first || (input.chord() && !second)) return false;

    const int written = input.chord()
        ? snprintf(out, out_size, "%s+%s", first->id, second->id)
        : snprintf(out, out_size, "%s", first->id);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool board_button_input_label(const ButtonInput &input,
                              char *out,
                              size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';

    const BoardButtonDefinition *first =
        board_button_find(input.first_button_key);
    const BoardButtonDefinition *second = input.chord()
        ? board_button_find(input.second_button_key)
        : nullptr;
    if (!first || (input.chord() && !second)) return false;

    const int written = input.chord()
        ? snprintf(out, out_size, "%s + %s", first->label, second->label)
        : snprintf(out, out_size, "%s", first->label);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace aircannect
