#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_button_manager.h"
#include "button_binding_config.h"
#include "local_action_router.h"

namespace aircannect {

class LocalInputController {
public:
    using InteractionHandler = void (*)(void *context);

    bool begin();
    void poll(uint32_t now_ms);

    void set_interaction_handler(InteractionHandler handler, void *context);
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
                                    ButtonInput input,
                                    ButtonGesture gesture,
                                    uint32_t now_ms);
    void dispatch(ButtonInput input,
                  ButtonGesture gesture,
                  uint32_t now_ms);

    BoardButtonManager buttons_;
    LocalActionRouter actions_;
    InteractionHandler interaction_handler_ = nullptr;
    void *interaction_context_ = nullptr;
};

}  // namespace aircannect
