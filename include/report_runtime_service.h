#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

class ReportBuildQueueService;
class ReportCacheFetchService;
class ReportCacheStorageRuntime;
class ReportEdfCatalogContext;
class ReportFetchRuntime;
class ReportPlotPrebuildService;
class ReportPrefetchService;
class ReportRangePlotBuilder;
class ReportResultBuildService;
class ReportResultCacheRuntime;
class ReportResultPrepareService;
class ReportSummaryRuntime;
class ReportSummaryService;

enum class ReportCacheFetchEvent : uint8_t;
enum class ReportSummaryFetchEvent : uint8_t;

class ReportRuntimeService {
public:
    ReportRuntimeService(ReportFetchRuntime &fetch,
                         ReportSummaryRuntime &summary_runtime,
                         ReportCacheStorageRuntime &cache_storage,
                         ReportResultCacheRuntime &result_cache,
                         ReportSummaryService &summary,
                         ReportCacheFetchService &cache_fetch,
                         ReportResultBuildService &result_build,
                         ReportRangePlotBuilder &range_plot,
                         ReportResultPrepareService &result_prepare,
                         ReportPlotPrebuildService &plot_prebuild,
                         ReportBuildQueueService &build_queue,
                         ReportPrefetchService &prefetch,
                         ReportEdfCatalogContext &edf_catalog);

    void poll(bool transport_backpressure_active,
              uint32_t rx_queue_full_alerts,
              bool therapy_running,
              bool stream_realtime_active);
    bool enqueue_spool_notification(const char *payload,
                                    size_t payload_len);

    bool busy() const;
    bool foreground_busy() const;
    bool background_work_active() const;
    bool cancel_cache_fetch();

private:
    struct SummaryPublishRetryState {
        uint32_t snapshot_generation = 0;
        uint32_t catalog_refresh_id = 0;
        uint32_t catalog_sessions = 0;
        uint32_t next_attempt_ms = 0;
        uint32_t next_warning_ms = 0;
        uint16_t failures = 0;
        uint16_t busy_retries = 0;
        bool catalog_pending = false;
        bool cache_invalidated = false;
    };

    // Pipeline service
    void service_build_queue(bool realtime_active);
    void service_range_plot(bool realtime_active);
    void handle_cache_fetch_event(ReportCacheFetchEvent event);
    void handle_summary_fetch_event(ReportSummaryFetchEvent event);

    // Summary snapshot publication
    void service_summary_snapshot_publish();

    // Pipeline dependencies
    ReportFetchRuntime &fetch_;
    ReportSummaryRuntime &summary_runtime_;
    ReportCacheStorageRuntime &cache_storage_;
    ReportResultCacheRuntime &result_cache_;
    ReportSummaryService &summary_;
    ReportCacheFetchService &cache_fetch_;
    ReportResultBuildService &result_build_;
    ReportRangePlotBuilder &range_plot_;
    ReportResultPrepareService &result_prepare_;
    ReportPlotPrebuildService &plot_prebuild_;
    ReportBuildQueueService &build_queue_;
    ReportPrefetchService &prefetch_;
    ReportEdfCatalogContext &edf_catalog_;

    // Summary snapshot publication state
    SummaryPublishRetryState summary_publish_retry_;
};

}  // namespace aircannect
