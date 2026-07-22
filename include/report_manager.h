#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_text_buffer.h"
#include "report_build_runtime.h"
#include "report_build_queue_service.h"
#include "report_cache_fetch_service.h"
#include "report_cache_maintenance_service.h"
#include "report_cache_storage_runtime.h"
#include "report_cache_write_service.h"
#include "report_cache_write_sink.h"
#include "report_edf_catalog_context.h"
#include "report_fetch_runtime.h"
#include "report_night_cache_service.h"
#include "report_night_coverage.h"
#include "report_night_index_runtime.h"
#include "report_night_index_service.h"
#include "report_night_query_service.h"
#include "report_plot_prebuild_service.h"
#include "report_prefetch_runtime.h"
#include "report_prefetch_service.h"
#include "report_range_plot_builder.h"
#include "report_range_plot_runtime.h"
#include "report_result_build_service.h"
#include "report_result_cache_runtime.h"
#include "report_result_prepare_service.h"
#include "report_result_serving_service.h"
#include "report_result_types.h"
#include "report_runtime_service.h"
#include "report_spool_types.h"
#include "report_summary_runtime.h"
#include "report_summary_service.h"
#include "report_summary_types.h"
#include "rpc_arbiter.h"

namespace aircannect {

class EdfReportCatalogJob;
struct EdfReportCatalogStatus;
struct ReportSeriesSample;
struct EdfReportRequiredRange;
struct ReportResolveScratch;

class ReportManager {
public:
    ReportManager();
    ~ReportManager();

    // Lifecycle and device events
    void begin();
    void set_edf_report_catalog(EdfReportCatalogJob *catalog);
    void poll(RpcArbiter &arbiter, bool therapy_running);
    bool handle_event(const RpcEvent &event);

    // Summary and night index
    bool request_summary_refresh(bool force = false);
    void build_summary_json(LargeTextBuffer &json) const;
    bool for_each_summary_night(ReportSummaryNightCallback callback,
                                void *context) const;
    bool summary_night_by_therapy_index(size_t therapy_index,
                                        ReportSummaryRecord &out) const;
    bool latest_summary_night(ReportSummaryRecord &out) const;
    ReportSummaryStatus summary_status() const;

    // Atomic mirror of summary status revision, published each poll() so the
    // background prefetch job can watch for new nights without a cross-task read.
    uint32_t summary_revision() const { return summary_.revision(); }

    // Coverage and spool cache
    bool night_coverage(uint64_t night_start_ms,
                        ReportNightCoverageStatus &out) const;

    bool request_night_cache(uint64_t night_start_ms, bool force = false);
    bool cancel_cache_fetch();

    bool clear_cache_all(ReportCacheClearResult &out);
    bool clear_cache_night(uint64_t night_start_ms, ReportCacheClearResult &out);
    bool clear_oldest_cache_nights(size_t max_nights,
                                   ReportCacheClearResult &out);
    bool prune_cache_to_latest_nights(size_t keep_latest,
                                      ReportCacheClearResult &out);

    // Work state and background service
    bool busy() const { return runtime_service_.busy(); }
    bool background_work_active() const;

    const ReportCacheFetchStatus &cache_fetch_status() const {
        return cache_fetch_.status();
    }

    // Report prefetch
    using PrefetchPhase = ReportPrefetchPhase;
    using PrefetchSnapshot = ReportPrefetchSnapshot;

    bool prefetch_request_candidate();
    void prefetch_preempt();
    PrefetchSnapshot prefetch_snapshot() const;

    // Idle plot prebuild and build queue
    using PlotPrebuildResult = ReportPlotPrebuildResult;
    PlotPrebuildResult request_idle_plot_prebuild();
    void preempt_idle_plot_prebuild();

    using BuildQueueSnapshot = ReportBuildQueueSnapshot;

    BuildQueueSnapshot build_queue_snapshot() const;
    bool edf_catalog_status(EdfReportCatalogStatus &out,
                            uint32_t timeout_ms = 0) const;

    bool foreground_busy() const;
    bool service_cache_writer();

    // Result materialization
    bool request_result_prepare_by_therapy_index(size_t therapy_index,
                                                 bool refresh_cache = false);
    bool request_result_prepare_by_start(uint64_t night_start_ms,
                                         bool refresh_cache = false);
    void build_result_chunks_json(LargeTextBuffer &json,
                                  size_t offset,
                                  size_t limit) const;

    // Result serving
    // Stateless per-night result read: serves a materialized LRU entry by night
    // index with an ETag, without touching the single build-scratch slot.
    using ResultRead = ReportResultRead;

    ResultRead read_result_by_start(uint64_t night_start_ms,
                                    const char *if_none_match,
                                    char *etag_out,
                                    size_t etag_out_size,
                                    LargeTextBuffer &json_out);

    // Plot serving
    // Stateless per-night plot read: serves the PSRAM blob for (night, version);
    // a miss queues a build and returns Building.
    using PlotRead = ReportPlotRead;

    PlotRead read_plot(size_t therapy_index, const char *version,
                       char *etag_out, size_t etag_out_size,
                       std::shared_ptr<ReportSpoolBuffer> &out);
    PlotRead read_plot_range(size_t therapy_index, const char *version,
                             char *etag_out, size_t etag_out_size,
                             int64_t from_ms, int64_t to_ms,
                             std::shared_ptr<ReportSpoolBuffer> &out);

    ReportResultStatus result_status() const;

    // Per-night content version "<start>-<dur>-<sessions>-<epoch>" for the HTTP
    // ETag; computed from the in-memory summary record + the per-night epoch (no SD).
    bool night_etag(size_t therapy_index, char *out, size_t out_size) const;

private:
    // Summary and night index state
    ReportSummaryRuntime summary_;
    mutable ReportNightIndexRuntime night_index_;

    // Spool summary and cache fetch state
    ReportFetchRuntime fetch_runtime_;

    // Write-side cache storage
    ReportCacheStorageRuntime cache_storage_;
    ReportCacheWriteSink cache_write_sink_;

    // Prefetch state
    mutable ReportPrefetchRuntime prefetch_;

    // Result build scratch
    ReportEdfCatalogContext edf_catalog_;

    // Indexed report nights
    mutable ReportNightIndexService night_index_service_;
    ReportNightQueryService night_query_;

    // Materialized result/range LRU and durable result cache writer
    ReportResultCacheRuntime result_cache_;

    // Summary spool fetch and published summary snapshot
    ReportSummaryService summary_service_;

    // Spool cache fetch execution
    ReportCacheFetchService cache_fetch_;

    // Write-side cache service
    ReportCacheWriteService cache_write_service_;

    // Active zoom range plot build state
    ReportRangePlotRuntime range_plot_runtime_;

    // Zoom range plot build state and policy
    ReportRangePlotBuilder range_plot_builder_;

    // Build queue and idle prebuild
    ReportBuildRuntime build_runtime_;

    // Result materialization and full-night plot build state
    ReportResultBuildService result_build_;

    // Result prepare orchestration
    ReportResultPrepareService result_prepare_;

    // Per-night cache coverage and fetch requests
    ReportNightCacheService night_cache_;

    // Report prefetch scheduling
    ReportPrefetchService prefetch_service_;

    // Idle full-night plot prebuild scheduling
    ReportPlotPrebuildService plot_prebuild_;

    // Result build queue servicing
    ReportBuildQueueService build_queue_service_;

    // Report cache reset/prune maintenance
    ReportCacheMaintenanceService cache_maintenance_;

    // Result/plot API serving policy
    ReportResultServingService result_serving_;

    // Report polling and foreground/background work policy
    ReportRuntimeService runtime_service_;
};

}  // namespace aircannect
