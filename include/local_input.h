#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class ButtonGesture : uint8_t {
    ShortPress = 1,
    LongPress = 2,
};

enum ButtonGestureMask : uint8_t {
    BUTTON_GESTURE_NONE = 0,
    BUTTON_GESTURE_SHORT = 1u << 0,
    BUTTON_GESTURE_LONG = 1u << 1,
};

enum class BoardButtonPull : uint8_t {
    None,
    Up,
    Down,
};

enum class LocalActionId : uint16_t {
    None = 0,
    DisplayPreviousPage = 1,
    DisplayNextPage = 2,
    DisplayToggleBacklight = 3,
    TherapyToggle = 4,
};

struct LocalActionDefinition {
    LocalActionId id = LocalActionId::None;
    const char *name = nullptr;
    const char *label = nullptr;
};

struct BoardButtonDefinition {
    uint16_t key = 0;
    const char *id = nullptr;
    const char *label = nullptr;
    int8_t gpio = -1;
    bool active_low = true;
    BoardButtonPull pull = BoardButtonPull::Up;
    uint8_t gestures = BUTTON_GESTURE_NONE;
    uint16_t debounce_ms = 35;
    uint16_t long_press_ms = 1200;
    LocalActionId short_action = LocalActionId::None;
    LocalActionId long_action = LocalActionId::None;
};

struct ButtonBinding {
    uint16_t button_key = 0;
    ButtonGesture gesture = ButtonGesture::ShortPress;
    LocalActionId action = LocalActionId::None;
};

const LocalActionDefinition *local_action_catalog(size_t &count);
const LocalActionDefinition *local_action_find(LocalActionId id);
const LocalActionDefinition *local_action_find(const char *name);
const char *button_gesture_name(ButtonGesture gesture);
bool parse_button_gesture(const char *value, ButtonGesture &gesture);

bool validate_button_catalog(const BoardButtonDefinition *buttons,
                             size_t count);
LocalActionId board_button_default_action(
    const BoardButtonDefinition &button,
    ButtonGesture gesture);
bool board_button_supports_gesture(const BoardButtonDefinition &button,
                                   ButtonGesture gesture);

}  // namespace aircannect
