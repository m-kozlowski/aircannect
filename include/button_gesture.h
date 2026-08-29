#pragma once

#include <stdint.h>

#include "local_input.h"

namespace aircannect {

class ButtonGestureState {
public:
    void configure(uint16_t debounce_ms, uint16_t long_press_ms);
    void reset(bool pressed, uint32_t now_ms);
    bool update(bool pressed, uint32_t now_ms, ButtonGesture &event);
    void suppress_current_press();

    bool armed() const { return armed_; }
    bool stable_pressed() const { return stable_pressed_; }
    bool press_handled() const { return long_press_emitted_; }

private:
    uint16_t debounce_ms_ = 35;
    uint16_t long_press_ms_ = 1200;
    bool raw_pressed_ = false;
    bool stable_pressed_ = false;
    bool armed_ = false;
    bool long_press_emitted_ = false;
    uint32_t raw_changed_ms_ = 0;
    uint32_t pressed_ms_ = 0;
};

class ButtonChordGestureState {
public:
    void begin(ButtonInput input,
               uint8_t gestures,
               uint16_t long_press_ms,
               uint32_t now_ms);
    void reset();
    bool update(bool first_pressed,
                bool second_pressed,
                uint32_t now_ms,
                ButtonGesture &event);

    bool active() const { return input_.chord(); }
    const ButtonInput &input() const { return input_; }

private:
    ButtonInput input_;
    uint8_t gestures_ = BUTTON_GESTURE_NONE;
    uint16_t long_press_ms_ = 0;
    uint32_t pressed_ms_ = 0;
    bool emitted_ = false;
};

}  // namespace aircannect
