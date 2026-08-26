#include "local_input_controller.h"

#include <vector>

#include "board_button.h"
#include "button_binding_config.h"
#include "debug_log.h"

namespace aircannect {

bool LocalInputController::begin() {
    size_t count = 0;
    const BoardButtonDefinition *catalog = board_button_catalog(count);
    if (!buttons_.begin(catalog, count)) return false;

    std::vector<ButtonBinding> defaults;
    const ButtonBindingConfig no_overrides;
    if (!resolve_button_bindings(catalog, count, no_overrides, defaults)) {
        return false;
    }

    actions_.apply_bindings(defaults.data(), defaults.size());
    buttons_.set_event_handler(handle_button_event, this);
    return true;
}

void LocalInputController::poll(uint32_t now_ms) {
    buttons_.poll(now_ms);
}

bool LocalInputController::register_action(
    LocalActionId action,
    LocalActionRouter::Handler handler,
    void *context) {
    return actions_.register_handler(action, handler, context);
}

bool LocalInputController::apply_config(const ButtonBindingConfig &config,
                                        uint32_t now_ms) {
    size_t count = 0;
    const BoardButtonDefinition *catalog = board_button_catalog(count);
    std::vector<ButtonBinding> resolved;
    if (!resolve_button_bindings(catalog, count, config, resolved)) {
        return false;
    }

    apply_bindings(resolved.data(), resolved.size(), now_ms);
    return true;
}

void LocalInputController::apply_bindings(const ButtonBinding *bindings,
                                          size_t count,
                                          uint32_t now_ms) {
    actions_.apply_bindings(bindings, count);
    buttons_.rearm(now_ms);
}

void LocalInputController::handle_button_event(void *context,
                                               uint16_t button_key,
                                               ButtonGesture gesture,
                                               uint32_t now_ms) {
    static_cast<LocalInputController *>(context)->dispatch(
        button_key, gesture, now_ms);
}

void LocalInputController::dispatch(uint16_t button_key,
                                    ButtonGesture gesture,
                                    uint32_t now_ms) {
    const LocalActionId action =
        actions_.effective_action(button_key, gesture);
    const LocalActionDefinition *definition = local_action_find(action);
    const bool accepted = actions_.dispatch(button_key, gesture, now_ms);

    Log::logf(CAT_GENERAL, accepted ? LOG_DEBUG : LOG_WARN,
              "[INPUT] button=%u gesture=%s action=%s %s\n",
              static_cast<unsigned>(button_key),
              button_gesture_name(gesture),
              definition ? definition->name : "unknown",
              accepted ? "accepted" : "rejected");
}

}  // namespace aircannect
