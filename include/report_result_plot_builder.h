#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_internal_types.h"

namespace aircannect {

class ReportResultCacheRuntime;
class ReportResultRuntime;

class ReportResultPlotBuilder {
public:
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    ReportResultPlotBuilder(ReportResultRuntime &result,
                            ReportResultCacheRuntime &cache);

    // Build state
    bool active() const;
    bool idle_prebuild_active() const;
    uint32_t elapsed_ms(uint32_t now_ms) const;
    uint64_t night_start_ms() const;

    // Lifecycle and preemption
    void reset();
    void abort_idle_prebuild(const char *reason);
    void preempt_idle_prebuild();
    void preempt_for_range(size_t therapy_index,
                           uint64_t night_start_ms);

    // Build execution
    bool start();
    void poll();

private:
    // Input processing
    bool process_event_chunk(const ReportResultChunk &chunk);
    bool process_series_chunk(size_t chunk_index);
    bool process_edf_series_batch(size_t seed_chunk_index, bool &processed);

    // Completion
    bool finish();
    void fail(const char *message);

    // Runtime state
    ReportResultRuntime &result_;
    ReportResultCacheRuntime &cache_;
};

}  // namespace aircannect
