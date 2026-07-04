#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_manager_internal_types.h"
#include "report_manager_limits.h"

namespace aircannect {

struct ReportBuildQueueSnapshot {
    bool available = false;
    bool lock_ok = false;
    size_t count = 0;
    uint64_t head_night_ms = 0;
    size_t head_therapy_index = 0;
    bool head_refresh = false;
    bool head_idle_prebuild = false;
    uint32_t head_age_ms = 0;
    uint64_t last_night_ms = 0;
    size_t last_therapy_index = 0;
    char last_outcome[16] = {};
    char last_state[16] = {};
    char last_error[48] = {};
    uint32_t enqueue_total = 0;
    uint32_t queued_total = 0;
    uint32_t already_total = 0;
    uint32_t service_total = 0;
    uint64_t last_enqueue_night_ms = 0;
    size_t last_enqueue_therapy_index = 0;
    char last_read[24] = {};
    char last_enqueue_result[24] = {};
    char last_service_block[24] = {};
};

class ReportBuildQueue {
public:
    using BuildQueueResult = report_manager_internal::BuildQueueResult;
    using ResultBuildJob = report_manager_internal::ResultBuildJob;

    ~ReportBuildQueue();

    bool begin();

    ReportBuildQueueSnapshot snapshot() const;
    BuildQueueResult enqueue(uint64_t night_start_ms,
                             size_t therapy_index,
                             bool refresh,
                             bool idle_prebuild);
    bool has_capacity() const;
    bool has_pending() const;
    void clear(uint64_t night_start_ms, bool all);

    void note_read(const char *state);
    void note_service_block(const char *reason);
    bool peek_head(ResultBuildJob &out) const;
    void note_service_started();
    void note_build_result(const ResultBuildJob &job,
                           const char *outcome,
                           const char *state,
                           const char *error);
    bool defer_head(const ResultBuildJob &job, uint32_t next_attempt_ms);
    bool pop_head(const ResultBuildJob &job);

private:
    SemaphoreHandle_t lock_ = nullptr;

    ResultBuildJob queue_[AC_REPORT_BUILD_QUEUE_MAX];
    size_t head_ = 0;
    size_t count_ = 0;

    uint32_t enqueue_total_ = 0;
    uint32_t queued_total_ = 0;
    uint32_t already_total_ = 0;
    uint32_t service_total_ = 0;

    uint64_t last_enqueue_night_ms_ = 0;
    size_t last_enqueue_therapy_index_ = 0;
    char last_read_[24] = {};
    char last_enqueue_result_[24] = {};
    char last_service_block_[24] = {};

    uint64_t last_night_ms_ = 0;
    size_t last_therapy_index_ = 0;
    char last_outcome_[16] = {};
    char last_state_[16] = {};
    char last_error_[48] = {};
};

}  // namespace aircannect
