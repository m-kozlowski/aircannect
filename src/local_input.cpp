#include "local_input.h"

#include <string.h>

namespace aircannect {

namespace {

constexpr LocalActionDefinition ACTIONS[] = {
    {LocalActionId::None, "none", "Disabled"},
    {LocalActionId::DisplayPreviousPage,
     "display_previous_page", "Previous display page"},
    {LocalActionId::DisplayNextPage,
     "display_next_page", "Next display page"},
    {LocalActionId::DisplayToggleBacklight,
     "display_toggle_backlight", "Toggle display backlight"},
    {LocalActionId::TherapyToggle,
     "therapy_toggle", "Start or stop therapy"},
};

}  // namespace

const LocalActionDefinition *local_action_catalog(size_t &count) {
    count = sizeof(ACTIONS) / sizeof(ACTIONS[0]);
    return ACTIONS;
}

const LocalActionDefinition *local_action_find(LocalActionId id) {
    size_t count = 0;
    const LocalActionDefinition *actions = local_action_catalog(count);
    for (size_t i = 0; i < count; ++i) {
        if (actions[i].id == id) return &actions[i];
    }
    return nullptr;
}

const LocalActionDefinition *local_action_find(const char *name) {
    if (!name) return nullptr;

    size_t count = 0;
    const LocalActionDefinition *actions = local_action_catalog(count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(actions[i].name, name) == 0) return &actions[i];
    }
    return nullptr;
}

const char *button_gesture_name(ButtonGesture gesture) {
    switch (gesture) {
        case ButtonGesture::ShortPress:
            return "short";
        case ButtonGesture::LongPress:
            return "long";
    }
    return "unknown";
}

bool parse_button_gesture(const char *value, ButtonGesture &gesture) {
    if (!value) return false;
    if (strcmp(value, "short") == 0) {
        gesture = ButtonGesture::ShortPress;
        return true;
    }
    if (strcmp(value, "long") == 0) {
        gesture = ButtonGesture::LongPress;
        return true;
    }
    return false;
}

bool validate_button_catalog(const BoardButtonDefinition *buttons,
                             size_t count) {
    if (count > 0 && !buttons) return false;

    for (size_t i = 0; i < count; ++i) {
        const BoardButtonDefinition &button = buttons[i];
        if (button.key == 0 || !button.id || !button.id[0] ||
            !button.label || !button.label[0] || button.gpio < 0 ||
            button.gestures == BUTTON_GESTURE_NONE ||
            button.debounce_ms == 0) {
            return false;
        }

        for (size_t other = 0; other < i; ++other) {
            if (buttons[other].key == button.key ||
                strcmp(buttons[other].id, button.id) == 0) {
                return false;
            }
        }
    }
    return true;
}

LocalActionId board_button_default_action(
    const BoardButtonDefinition &button,
    ButtonGesture gesture) {
    switch (gesture) {
        case ButtonGesture::ShortPress:
            return button.short_action;
        case ButtonGesture::LongPress:
            return button.long_action;
    }
    return LocalActionId::None;
}

bool board_button_supports_gesture(const BoardButtonDefinition &button,
                                   ButtonGesture gesture) {
    switch (gesture) {
        case ButtonGesture::ShortPress:
            return (button.gestures & BUTTON_GESTURE_SHORT) != 0;
        case ButtonGesture::LongPress:
            return (button.gestures & BUTTON_GESTURE_LONG) != 0;
    }
    return false;
}

}  // namespace aircannect
