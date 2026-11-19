#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "local_input.h"

namespace aircannect {

class LocalActionRouter {
public:
    // Null means accepted; other results describe the rejection.
    using Handler = const char * (*)(void *context, uint32_t now_ms);

    bool register_handler(LocalActionId action,
                          Handler handler,
                          void *context);
    const char *dispatch(LocalActionId action, uint32_t now_ms) const;

private:
    struct HandlerEntry {
        LocalActionId action = LocalActionId::None;
        Handler handler = nullptr;
        void *context = nullptr;
    };

    std::vector<HandlerEntry> handlers_;
};

}  // namespace aircannect
