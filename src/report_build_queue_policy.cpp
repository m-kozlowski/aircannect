#include "report_build_queue_policy.h"

namespace aircannect {
namespace report_manager_internal {
namespace {

bool job_ready(const ResultBuildJob &job, uint32_t now_ms) {
    return job.next_attempt_ms == 0 ||
           static_cast<int32_t>(now_ms - job.next_attempt_ms) >= 0;
}

}  // namespace

BuildQueueSelection select_result_build_job(const ResultBuildJob *jobs,
                                             size_t count,
                                             uint32_t now_ms,
                                             size_t &selected_index) {
    selected_index = 0;
    if (!jobs || count == 0) return BuildQueueSelection::Empty;

    bool foreground_pending = false;
    for (size_t i = 0; i < count; ++i) {
        if (!jobs[i].idle_prebuild) {
            foreground_pending = true;
            break;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        if (foreground_pending && jobs[i].idle_prebuild) continue;
        if (!foreground_pending && !jobs[i].idle_prebuild) continue;
        if (!job_ready(jobs[i], now_ms)) continue;

        selected_index = i;
        return BuildQueueSelection::Ready;
    }

    return BuildQueueSelection::Waiting;
}

uint32_t result_build_retry_delay_ms(uint8_t retry_attempt) {
    static constexpr uint32_t RETRY_DELAYS_MS[] = {
        250,
        500,
        1000,
        2000,
        5000,
        10000,
    };

    if (retry_attempt == 0) return RETRY_DELAYS_MS[0];
    const size_t last = sizeof(RETRY_DELAYS_MS) /
                        sizeof(RETRY_DELAYS_MS[0]) - 1;
    size_t index = retry_attempt - 1;
    if (index > last) index = last;
    return RETRY_DELAYS_MS[index];
}

}  // namespace report_manager_internal
}  // namespace aircannect
