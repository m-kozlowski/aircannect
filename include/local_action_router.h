#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "local_input.h"

namespace aircannect {

enum class ActionSource : uint8_t { Button, Schedule };

struct ActionInvocation {
    ActionSource source;
    uint32_t now_ms;
};

class LocalActionRouter {
public:
    // Null means accepted; otherwise return a stable, static reason string.
    using Handler = const char * (*)(void *context, ActionInvocation call);

    bool register_handler(LocalActionId action,
                          Handler handler,
                          void *context);
    const char *dispatch(LocalActionId action, ActionInvocation call) const;

private:
    struct HandlerEntry {
        LocalActionId action = LocalActionId::None;
        Handler handler = nullptr;
        void *context = nullptr;
    };

    std::vector<HandlerEntry> handlers_;
};

}  // namespace aircannect
