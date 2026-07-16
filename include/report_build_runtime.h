#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_build_queue.h"
#include "report_night_index_cache.h"
#include "report_plot_prebuild_state.h"

namespace aircannect {

class ReportBuildRuntime {
public:
    using BuildQueueResult = ReportBuildQueue::BuildQueueResult;
    using BuildQueueSelection = ReportBuildQueue::BuildQueueSelection;
    using BuildQueueDeferResult = ReportBuildQueue::BuildQueueDeferResult;
    using ResultBuildJob = ReportBuildQueue::ResultBuildJob;

    bool begin();

    // Result build queue
    ReportBuildQueueSnapshot snapshot() const;
    BuildQueueResult enqueue(uint64_t night_start_ms,
                             size_t therapy_index,
                             bool refresh,
                             bool idle_prebuild);
    bool has_capacity() const;
    bool has_pending() const;
    bool has_foreground_pending() const;
    void clear(uint64_t night_start_ms, bool all);

    void note_read(const char *state);
    void note_service_block(const char *reason);
    BuildQueueSelection select_next(uint32_t now_ms,
                                    ResultBuildJob &out) const;
    void note_service_started();
    void note_build_result(const ResultBuildJob &job,
                           const char *outcome,
                           const char *state,
                           const char *error);
    BuildQueueDeferResult defer(const ResultBuildJob &job,
                                bool retry,
                                uint32_t now_ms);
    bool remove(const ResultBuildJob &job);

    // Idle prebuild cursor
    bool prebuild_key_matches(const ReportNightIndexCacheKey &key) const;
    void reset_prebuild_for_key(const ReportNightIndexCacheKey &key);
    bool prebuild_rescan_delay_active(uint32_t now_ms) const;
    void mark_prebuild_drained(uint32_t now_ms, uint32_t delay_ms);

    size_t prebuild_cursor() const;
    void advance_prebuild_cursor();
    void rewind_prebuild_cursor();

private:
    ReportBuildQueue queue_;
    ReportPlotPrebuildState prebuild_;
};

}  // namespace aircannect
