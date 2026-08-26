#pragma once

#include <stdint.h>

#include "local_input.h"

namespace aircannect {

class ButtonGestureState {
public:
    void configure(uint16_t debounce_ms, uint16_t long_press_ms);
    void reset(bool pressed, uint32_t now_ms);
    bool update(bool pressed, uint32_t now_ms, ButtonGesture &event);

    bool armed() const { return armed_; }

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

}  // namespace aircannect
