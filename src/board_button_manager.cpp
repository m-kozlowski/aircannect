#include "board_button_manager.h"

#include <Arduino.h>

namespace aircannect {

bool BoardButtonManager::begin(const BoardButtonDefinition *buttons,
                               size_t count) {
    buttons_.clear();
    if (!validate_button_catalog(buttons, count)) return false;

    buttons_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const BoardButtonDefinition &definition = buttons[i];
        uint8_t mode = INPUT;
        if (definition.pull == BoardButtonPull::Up) mode = INPUT_PULLUP;
        if (definition.pull == BoardButtonPull::Down) mode = INPUT_PULLDOWN;
        pinMode(definition.gpio, mode);

        RuntimeButton runtime;
        runtime.definition = definition;
        runtime.gesture.configure(definition.debounce_ms,
                                  definition.long_press_ms);
        runtime.gesture.reset(read_pressed(definition), millis());
        buttons_.push_back(runtime);
    }
    return true;
}

void BoardButtonManager::set_event_handler(EventHandler handler,
                                           void *context) {
    event_handler_ = handler;
    event_context_ = context;
}

void BoardButtonManager::poll(uint32_t now_ms) {
    for (RuntimeButton &button : buttons_) {
        ButtonGesture event = ButtonGesture::ShortPress;
        if (!button.gesture.update(read_pressed(button.definition),
                                   now_ms, event)) {
            continue;
        }
        if (!board_button_supports_gesture(button.definition, event)) {
            continue;
        }
        if (event_handler_) {
            event_handler_(event_context_, button.definition.key,
                           event, now_ms);
        }
    }
}

void BoardButtonManager::rearm(uint32_t now_ms) {
    for (RuntimeButton &button : buttons_) {
        button.gesture.reset(read_pressed(button.definition), now_ms);
    }
}

bool BoardButtonManager::read_pressed(
    const BoardButtonDefinition &button) const {
    const bool high = digitalRead(button.gpio) == HIGH;
    return button.active_low ? !high : high;
}

}  // namespace aircannect
