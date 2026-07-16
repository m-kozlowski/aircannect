#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {
namespace report_manager_internal {

struct ResultBuildJob {
    uint64_t night_start_ms = 0;
    size_t therapy_index = 0;
    bool refresh = false;
    bool idle_prebuild = false;
    uint32_t queued_ms = 0;
    uint32_t next_attempt_ms = 0;
    uint32_t token = 0;
    uint16_t defer_count = 0;
    uint8_t retry_attempts = 0;
};

enum class BuildQueueResult : uint8_t {
    Queued,
    AlreadyQueued,
    Full,
    Unavailable,
};

enum class BuildQueueSelection : uint8_t {
    Empty,
    Waiting,
    Ready,
};

enum class BuildQueueDeferResult : uint8_t {
    Deferred,
    RetryExhausted,
    Stale,
};

BuildQueueSelection select_result_build_job(const ResultBuildJob *jobs,
                                             size_t count,
                                             uint32_t now_ms,
                                             size_t &selected_index);
uint32_t result_build_retry_delay_ms(uint8_t retry_attempt);

}  // namespace report_manager_internal
}  // namespace aircannect
