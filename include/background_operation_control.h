#pragma once

#include <stdint.h>

namespace aircannect {

enum class BackgroundOperationStop : uint8_t {
    None,
    Aborted,
    Deadline,
};

using BackgroundOperationAbortCallback = bool (*)(void *ctx);

struct BackgroundOperationControl {
    uint32_t started_ms = 0;
    uint32_t timeout_ms = 0;
    BackgroundOperationAbortCallback should_abort = nullptr;
    void *ctx = nullptr;

    BackgroundOperationStop stop_reason(uint32_t now_ms) const {
        if (should_abort && should_abort(ctx)) {
            return BackgroundOperationStop::Aborted;
        }
        if (timeout_ms != 0 &&
            static_cast<uint32_t>(now_ms - started_ms) >= timeout_ms) {
            return BackgroundOperationStop::Deadline;
        }
        return BackgroundOperationStop::None;
    }
};

const char *background_operation_stop_error(BackgroundOperationStop reason);

inline bool background_operation_result_current(uint32_t started_generation,
                                                uint32_t current_generation,
                                                bool abort_requested) {
    return !abort_requested && started_generation == current_generation;
}

}  // namespace aircannect
