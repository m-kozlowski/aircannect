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

const char *LocalActionRouter::dispatch(LocalActionId action,
                                       ActionInvocation call) const {
    if (action == LocalActionId::None) return nullptr;

    for (const HandlerEntry &entry : handlers_) {
        if (entry.action == action && entry.handler) {
            return entry.handler(entry.context, call);
        }
    }
    return "action_unavailable";
}

}  // namespace aircannect
