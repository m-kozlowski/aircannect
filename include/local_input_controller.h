#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_button_manager.h"
#include "button_binding_config.h"
#include "local_action_router.h"

namespace aircannect {

class LocalInputController {
public:
    bool begin();
    void poll(uint32_t now_ms);

    bool register_action(LocalActionId action,
                         LocalActionRouter::Handler handler,
                         void *context);
    bool apply_config(const ButtonBindingConfig &config,
                      uint32_t now_ms);
    void apply_bindings(const ButtonBinding *bindings,
                        size_t count,
                        uint32_t now_ms);

    bool available() const { return buttons_.available(); }

private:
    static void handle_button_event(void *context,
                                    uint16_t button_key,
                                    ButtonGesture gesture,
                                    uint32_t now_ms);
    void dispatch(uint16_t button_key,
                  ButtonGesture gesture,
                  uint32_t now_ms);

    BoardButtonManager buttons_;
    LocalActionRouter actions_;
};

}  // namespace aircannect
