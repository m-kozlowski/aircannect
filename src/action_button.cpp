#include "action_button.h"

#include <Arduino.h>

#include "board_button.h"

namespace aircannect {

bool ActionButton::begin() {
#if AC_ACTION_BUTTON_GPIO < 0
    return true;
#else
    pinMode(AC_ACTION_BUTTON_GPIO,
            AC_ACTION_BUTTON_PULLUP ? INPUT_PULLUP : INPUT);

    available_ = true;
    raw_pressed_ = read_pressed();
    stable_pressed_ = raw_pressed_;
    raw_changed_ms_ = millis();
    if (stable_pressed_) pressed_ms_ = raw_changed_ms_;
    return true;
#endif
}

ActionButtonEvent ActionButton::poll(uint32_t now_ms) {
    if (!available_) return ActionButtonEvent::None;

    const bool pressed = read_pressed();
    if (pressed != raw_pressed_) {
        raw_pressed_ = pressed;
        raw_changed_ms_ = now_ms;
    }

    if (raw_pressed_ != stable_pressed_ &&
        now_ms - raw_changed_ms_ >= AC_ACTION_BUTTON_DEBOUNCE_MS) {
        stable_pressed_ = raw_pressed_;

        if (stable_pressed_) {
            pressed_ms_ = now_ms;
            long_press_emitted_ = false;
        } else if (!long_press_emitted_) {
            return ActionButtonEvent::ShortPress;
        }
    }

    if (stable_pressed_ && !long_press_emitted_ &&
        now_ms - pressed_ms_ >= AC_ACTION_BUTTON_LONG_PRESS_MS) {
        long_press_emitted_ = true;
        return ActionButtonEvent::LongPress;
    }

    return ActionButtonEvent::None;
}

bool ActionButton::read_pressed() const {
#if AC_ACTION_BUTTON_GPIO < 0
    return false;
#else
    const bool high = digitalRead(AC_ACTION_BUTTON_GPIO) == HIGH;
    return AC_ACTION_BUTTON_ACTIVE_LOW ? !high : high;
#endif
}

}  // namespace aircannect
