#pragma once

#include <stdint.h>

namespace aircannect {

enum class ActionButtonEvent : uint8_t {
    None,
    ShortPress,
    LongPress,
};

class ActionButton {
public:
    bool begin();
    ActionButtonEvent poll(uint32_t now_ms);
    bool available() const { return available_; }

private:
    bool read_pressed() const;

    bool available_ = false;
    bool raw_pressed_ = false;
    bool stable_pressed_ = false;
    bool long_press_emitted_ = false;
    uint32_t raw_changed_ms_ = 0;
    uint32_t pressed_ms_ = 0;
};

}  // namespace aircannect
