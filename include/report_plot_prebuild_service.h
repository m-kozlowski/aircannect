#pragma once

#include <stdint.h>

#include "report_build_runtime.h"
#include "report_cache_fetch_service.h"
#include "report_night_index_service.h"
#include "report_range_plot_builder.h"
#include "report_result_build_service.h"
#include "report_result_cache_runtime.h"
#include "report_summary_service.h"

namespace aircannect {

enum class ReportPlotPrebuildResult : uint8_t {
    Queued,
    AlreadyQueued,
    Scanned,
    Drained,
    Waiting,
    Unavailable,
};

class ReportPlotPrebuildService {
public:
    ReportPlotPrebuildService(ReportSummaryService &summary,
                              ReportCacheFetchService &cache_fetch,
                              ReportResultBuildService &result_build,
                              ReportRangePlotBuilder &range_plot,
                              ReportResultCacheRuntime &result_cache,
                              ReportNightIndexService &night_index,
                              ReportBuildRuntime &build);

    bool gate_open(const char **reason = nullptr) const;
    ReportPlotPrebuildResult request();
    void preempt();

private:
    ReportSummaryService &summary_;
    ReportCacheFetchService &cache_fetch_;
    ReportResultBuildService &result_build_;
    ReportRangePlotBuilder &range_plot_;
    ReportResultCacheRuntime &result_cache_;
    ReportNightIndexService &night_index_;
    ReportBuildRuntime &build_;
};

}  // namespace aircannect
