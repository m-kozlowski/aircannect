#pragma once

#include <stdint.h>

#include "report_prefetch_state.h"

namespace aircannect {

class ReportCacheFetchService;
class ReportEdfCatalogContext;
class ReportNightCacheService;
class ReportPrefetchRuntime;
class ReportRangePlotBuilder;
class ReportResultBuildService;
class ReportSummaryService;

class ReportPrefetchService {
public:
    ReportPrefetchService(ReportPrefetchRuntime &prefetch,
                          ReportCacheFetchService &cache_fetch,
                          ReportNightCacheService &night_cache,
                          ReportSummaryService &summary,
                          ReportResultBuildService &result_build,
                          ReportRangePlotBuilder &range_plot,
                          ReportEdfCatalogContext &edf_catalog);

    bool request_candidate();
    void preempt();
    ReportPrefetchSnapshot snapshot() const;

    void yield_to_foreground();
    void service(bool realtime_active);

    bool foreground_busy() const;
    bool work_active() const;

private:
    void set_phase(ReportPrefetchPhase phase,
                   uint64_t night_ms,
                   bool inc_completed,
                   bool inc_failed);
    bool busy() const;

    ReportPrefetchRuntime &prefetch_;
    ReportCacheFetchService &cache_fetch_;
    ReportNightCacheService &night_cache_;
    ReportSummaryService &summary_;
    ReportResultBuildService &result_build_;
    ReportRangePlotBuilder &range_plot_;
    ReportEdfCatalogContext &edf_catalog_;
};

}  // namespace aircannect
