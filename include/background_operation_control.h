#pragma once

#include <stdint.h>

namespace aircannect {

enum class BackgroundOperationStop : uint8_t {
    None,
    Aborted,
    Deadline,
};

enum class BackgroundOperationTimeoutPolicy : uint8_t {
    TotalDuration,
    NoProgress,
};

using BackgroundOperationAbortCallback = bool (*)(void *ctx);

struct BackgroundOperationControl {
    uint32_t started_ms = 0;
    uint32_t last_progress_ms = 0;
    uint32_t timeout_ms = 0;
    bool progress_observed = false;
    BackgroundOperationTimeoutPolicy timeout_policy =
        BackgroundOperationTimeoutPolicy::TotalDuration;
    BackgroundOperationAbortCallback should_abort = nullptr;
    void *ctx = nullptr;

    void note_progress(uint32_t now_ms) {
        last_progress_ms = now_ms;
        progress_observed = true;
    }

    BackgroundOperationStop stop_reason(uint32_t now_ms) const {
        if (should_abort && should_abort(ctx)) {
            return BackgroundOperationStop::Aborted;
        }
        const uint32_t timeout_start =
            timeout_policy == BackgroundOperationTimeoutPolicy::NoProgress &&
                    progress_observed
                ? last_progress_ms
                : started_ms;
        if (timeout_ms != 0 &&
            static_cast<uint32_t>(now_ms - timeout_start) >= timeout_ms) {
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
