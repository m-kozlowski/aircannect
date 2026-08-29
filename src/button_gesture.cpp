#include "button_gesture.h"

namespace aircannect {

void ButtonGestureState::configure(uint16_t debounce_ms,
                                   uint16_t long_press_ms) {
    debounce_ms_ = debounce_ms;
    long_press_ms_ = long_press_ms;
}

void ButtonGestureState::reset(bool pressed, uint32_t now_ms) {
    raw_pressed_ = pressed;
    stable_pressed_ = pressed;
    armed_ = false;
    long_press_emitted_ = false;
    raw_changed_ms_ = now_ms;
    pressed_ms_ = pressed ? now_ms : 0;
}

bool ButtonGestureState::update(bool pressed,
                                uint32_t now_ms,
                                ButtonGesture &event) {
    if (pressed != raw_pressed_) {
        raw_pressed_ = pressed;
        raw_changed_ms_ = now_ms;
    }

    if (raw_pressed_ != stable_pressed_ &&
        now_ms - raw_changed_ms_ >= debounce_ms_) {
        stable_pressed_ = raw_pressed_;

        if (stable_pressed_) {
            pressed_ms_ = now_ms;
            long_press_emitted_ = false;
        } else if (!armed_) {
            armed_ = true;
        } else if (!long_press_emitted_) {
            event = ButtonGesture::ShortPress;
            return true;
        }
    }

    if (!armed_ && !stable_pressed_ &&
        now_ms - raw_changed_ms_ >= debounce_ms_) {
        armed_ = true;
    }

    if (!armed_ || !stable_pressed_ || long_press_emitted_ ||
        long_press_ms_ == 0) {
        return false;
    }
    if (now_ms - pressed_ms_ < long_press_ms_) return false;

    long_press_emitted_ = true;
    event = ButtonGesture::LongPress;
    return true;
}

void ButtonGestureState::suppress_current_press() {
    if (stable_pressed_) long_press_emitted_ = true;
}

void ButtonChordGestureState::begin(ButtonInput input,
                                    uint16_t long_press_ms,
                                    uint32_t now_ms) {
    input_ = button_input(input.first_button_key,
                          input.second_button_key);
    long_press_ms_ = long_press_ms;
    pressed_ms_ = now_ms;
    emitted_ = false;
}

void ButtonChordGestureState::reset() {
    input_ = {};
    long_press_ms_ = 0;
    pressed_ms_ = 0;
    emitted_ = false;
}

bool ButtonChordGestureState::update(bool first_pressed,
                                     bool second_pressed,
                                     uint32_t now_ms,
                                     ButtonGesture &event) {
    if (!active()) return false;

    if (!emitted_ && first_pressed && second_pressed &&
        now_ms - pressed_ms_ >= long_press_ms_) {
        emitted_ = true;
        event = ButtonGesture::LongPress;
        return true;
    }
    if (!emitted_ && (!first_pressed || !second_pressed)) {
        emitted_ = true;
        event = ButtonGesture::ShortPress;
        return true;
    }

    if (!first_pressed && !second_pressed) reset();
    return false;
}

}  // namespace aircannect
