#include "board_button_manager.h"

#include <Arduino.h>

namespace aircannect {

bool BoardButtonManager::begin(const BoardButtonDefinition *buttons,
                               size_t count) {
    buttons_.clear();
    pending_events_.clear();
    chord_.reset();
    if (!validate_button_catalog(buttons, count)) return false;

    buttons_.reserve(count);
    pending_events_.reserve(count);
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
    pending_events_.clear();
    for (RuntimeButton &button : buttons_) {
        ButtonGesture event = ButtonGesture::ShortPress;
        if (!button.gesture.update(read_pressed(button.definition),
                                   now_ms, event)) {
            continue;
        }
        if (!board_button_supports_gesture(button.definition, event)) {
            continue;
        }
        pending_events_.push_back(
            {button_input(button.definition.key), event});
    }

    poll_chord(now_ms);

    for (const PendingButtonEvent &event : pending_events_) {
        emit(event.input, event.gesture, now_ms);
    }
}

void BoardButtonManager::rearm(uint32_t now_ms) {
    chord_.reset();
    pending_events_.clear();
    for (RuntimeButton &button : buttons_) {
        button.gesture.reset(read_pressed(button.definition), now_ms);
    }
}

BoardButtonManager::RuntimeButton *BoardButtonManager::find_runtime(
    uint16_t button_key) {
    for (RuntimeButton &button : buttons_) {
        if (button.definition.key == button_key) return &button;
    }
    return nullptr;
}

void BoardButtonManager::poll_chord(uint32_t now_ms) {
    if (chord_.active()) {
        RuntimeButton *first =
            find_runtime(chord_.input().first_button_key);
        RuntimeButton *second =
            find_runtime(chord_.input().second_button_key);
        if (!first || !second) {
            chord_.reset();
            return;
        }

        const bool first_pressed = first->gesture.stable_pressed();
        const bool second_pressed = second->gesture.stable_pressed();
        ButtonGesture event = ButtonGesture::ShortPress;
        const ButtonInput input = chord_.input();
        if (chord_.update(first_pressed, second_pressed, now_ms, event)) {
            emit(input, event, now_ms);
        }
        return;
    }

    RuntimeButton *pressed[2] = {};
    size_t pressed_count = 0;
    for (RuntimeButton &button : buttons_) {
        if (!button.gesture.stable_pressed()) continue;
        if (pressed_count < 2) pressed[pressed_count] = &button;
        pressed_count++;
    }
    if (pressed_count != 2 || !pressed[0] || !pressed[1] ||
        pressed[0]->gesture.press_handled() ||
        pressed[1]->gesture.press_handled()) {
        return;
    }

    const ButtonInput input = button_input(pressed[0]->definition.key,
                                           pressed[1]->definition.key);
    const uint16_t long_press_ms =
        pressed[0]->definition.long_press_ms >
                pressed[1]->definition.long_press_ms
            ? pressed[0]->definition.long_press_ms
            : pressed[1]->definition.long_press_ms;
    chord_.begin(input, long_press_ms, now_ms);

    pressed[0]->gesture.suppress_current_press();
    pressed[1]->gesture.suppress_current_press();
    pending_events_.clear();
}

void BoardButtonManager::emit(ButtonInput input,
                              ButtonGesture gesture,
                              uint32_t now_ms) const {
    if (event_handler_) {
        event_handler_(event_context_, input, gesture, now_ms);
    }
}

bool BoardButtonManager::read_pressed(
    const BoardButtonDefinition &button) const {
    const bool high = digitalRead(button.gpio) == HIGH;
    return button.active_low ? !high : high;
}

}  // namespace aircannect
