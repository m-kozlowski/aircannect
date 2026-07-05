#pragma once

#include "report_build_runtime.h"
#include "report_night_index_service.h"
#include "report_result_cache_runtime.h"

namespace aircannect {

class ReportBuildQueueService {
public:
    using ResultBuildJob = ReportBuildRuntime::ResultBuildJob;

    ReportBuildQueueService(ReportBuildRuntime &build,
                            ReportNightIndexService &night_index,
                            ReportResultCacheRuntime &result_cache);

    // Queue state
    ReportBuildQueueSnapshot snapshot() const;
    bool has_pending() const;
    bool has_foreground_pending() const;
    void clear(uint64_t night_start_ms, bool all);

    // Runtime service accounting
    void note_service_block(const char *reason);
    bool peek_head(ResultBuildJob &out) const;
    void note_service_started();
    void note_build_result(const ResultBuildJob &job,
                           const char *outcome,
                           const char *state,
                           const char *error);
    bool defer_head(const ResultBuildJob &job, uint32_t next_attempt_ms);
    bool pop_head(const ResultBuildJob &job);

    // User/API enqueue
    bool request_prepare_by_therapy_index(size_t therapy_index,
                                          bool refresh_cache);
    bool request_prepare_by_start(uint64_t night_start_ms,
                                  bool refresh_cache);

private:
    ReportBuildRuntime &build_;
    ReportNightIndexService &night_index_;
    ReportResultCacheRuntime &result_cache_;
};

}  // namespace aircannect
