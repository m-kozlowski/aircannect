#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "button_gesture.h"
#include "local_input.h"

namespace aircannect {

class BoardButtonManager {
public:
    using EventHandler = void (*)(void *context,
                                  uint16_t button_key,
                                  ButtonGesture gesture,
                                  uint32_t now_ms);

    bool begin(const BoardButtonDefinition *buttons, size_t count);
    void set_event_handler(EventHandler handler, void *context);
    void poll(uint32_t now_ms);
    void rearm(uint32_t now_ms);

    bool available() const { return !buttons_.empty(); }

private:
    struct RuntimeButton {
        BoardButtonDefinition definition;
        ButtonGestureState gesture;
    };

    bool read_pressed(const BoardButtonDefinition &button) const;

    std::vector<RuntimeButton> buttons_;
    EventHandler event_handler_ = nullptr;
    void *event_context_ = nullptr;
};

}  // namespace aircannect
