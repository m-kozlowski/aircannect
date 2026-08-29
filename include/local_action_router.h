#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "local_input.h"

namespace aircannect {

class LocalActionRouter {
public:
    using Handler = bool (*)(void *context, uint32_t now_ms);

    bool register_handler(LocalActionId action,
                          Handler handler,
                          void *context);
    void apply_bindings(const ButtonBinding *bindings, size_t count);
    bool dispatch(ButtonInput input,
                  ButtonGesture gesture,
                  uint32_t now_ms) const;

    LocalActionId effective_action(ButtonInput input,
                                   ButtonGesture gesture) const;

private:
    struct HandlerEntry {
        LocalActionId action = LocalActionId::None;
        Handler handler = nullptr;
        void *context = nullptr;
    };

    std::vector<ButtonBinding> bindings_;
    std::vector<HandlerEntry> handlers_;
};

}  // namespace aircannect
