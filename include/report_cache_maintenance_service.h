#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class ReportBuildQueueService;
class ReportCacheFetchService;
class ReportNightIndexRuntime;
class ReportNightIndexService;
class ReportRangePlotBuilder;
class ReportResultBuildService;
class ReportResultCacheRuntime;
class ReportResultPrepareService;
class ReportSummaryRuntime;
class ReportSummaryService;

struct ReportCacheClearResult {
    uint32_t store_reset = 0;
    uint32_t summary_deleted = 0;
    uint32_t nights_cleared = 0;
    uint32_t chunks_deleted = 0;
    uint32_t coverage_deleted = 0;
    uint32_t plots_deleted = 0;
    uint32_t result_json_deleted = 0;
};

class ReportCacheMaintenanceService {
public:
    ReportCacheMaintenanceService(ReportSummaryRuntime &summary_runtime,
                                  ReportNightIndexRuntime &night_index_runtime,
                                  ReportNightIndexService &night_index,
                                  ReportSummaryService &summary,
                                  ReportCacheFetchService &cache_fetch,
                                  ReportResultCacheRuntime &result_cache,
                                  ReportResultBuildService &result_build,
                                  ReportRangePlotBuilder &range_plot,
                                  ReportResultPrepareService &result_prepare,
                                  ReportBuildQueueService &build_queue);

    bool clear_all(ReportCacheClearResult &out);
    bool clear_night(uint64_t night_start_ms, ReportCacheClearResult &out);
    bool clear_oldest_nights(size_t max_nights,
                             ReportCacheClearResult &out);
    bool prune_to_latest_nights(size_t keep_latest,
                                ReportCacheClearResult &out);

private:
    bool active_work() const;
    bool clear_range(int64_t start_ms,
                     int64_t end_ms,
                     ReportCacheClearResult &out);

    ReportSummaryRuntime &summary_runtime_;
    ReportNightIndexRuntime &night_index_runtime_;
    ReportNightIndexService &night_index_;
    ReportSummaryService &summary_;
    ReportCacheFetchService &cache_fetch_;
    ReportResultCacheRuntime &result_cache_;
    ReportResultBuildService &result_build_;
    ReportRangePlotBuilder &range_plot_;
    ReportResultPrepareService &result_prepare_;
    ReportBuildQueueService &build_queue_;
};

}  // namespace aircannect
