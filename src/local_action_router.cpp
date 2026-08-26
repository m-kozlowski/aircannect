#include "local_action_router.h"

namespace aircannect {

bool LocalActionRouter::register_handler(LocalActionId action,
                                         Handler handler,
                                         void *context) {
    if (action == LocalActionId::None || !handler ||
        !local_action_find(action)) {
        return false;
    }

    for (HandlerEntry &entry : handlers_) {
        if (entry.action != action) continue;
        entry.handler = handler;
        entry.context = context;
        return true;
    }

    handlers_.push_back({action, handler, context});
    return true;
}

void LocalActionRouter::apply_bindings(const ButtonBinding *bindings,
                                       size_t count) {
    std::vector<ButtonBinding> next;
    if (bindings && count > 0) {
        next.assign(bindings, bindings + count);
    }
    bindings_.swap(next);
}

LocalActionId LocalActionRouter::effective_action(
    uint16_t button_key,
    ButtonGesture gesture) const {
    for (const ButtonBinding &binding : bindings_) {
        if (binding.button_key == button_key &&
            binding.gesture == gesture) {
            return binding.action;
        }
    }
    return LocalActionId::None;
}

bool LocalActionRouter::dispatch(uint16_t button_key,
                                 ButtonGesture gesture,
                                 uint32_t now_ms) const {
    const LocalActionId action = effective_action(button_key, gesture);
    if (action == LocalActionId::None) return true;

    for (const HandlerEntry &entry : handlers_) {
        if (entry.action == action && entry.handler) {
            return entry.handler(entry.context, now_ms);
        }
    }
    return false;
}

}  // namespace aircannect
