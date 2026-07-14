#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_edf_plot_batch.h"
#include "report_manager_internal_types.h"

namespace aircannect {

class ReportEdfCatalogContext;
class ReportNightIndexService;
class ReportRangePlotRuntime;
class ReportResultCacheRuntime;
struct ReportIndexedNight;
struct ReportProviderChunk;
struct ReportResolvedPlan;

class ReportRangePlotBuilder {
public:
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    ReportRangePlotBuilder(ReportRangePlotRuntime &range_plot,
                           ReportResultCacheRuntime &cache,
                           ReportNightIndexService &night_index,
                           ReportEdfCatalogContext &edf_catalog);

    bool active() const;
    bool matches(size_t index,
                 uint64_t night_start_ms,
                 int64_t from_ms,
                 int64_t to_ms) const;

    void reset(bool clear_ready);
    bool start(uint64_t night_start_ms,
               size_t therapy_index_hint,
               int64_t from_ms,
               int64_t to_ms,
               bool &waiting_for_result);
    void poll();

private:
    // Provider callbacks
    static bool collect_chunk(void *context,
                              const ReportProviderChunk &provider_chunk);

    // Plan materialization
    bool add_provider_chunk(const ReportProviderChunk &provider_chunk,
                            size_t stream_index);
    bool materialize_plan(const ReportIndexedNight &night,
                          const ReportResolvedPlan &plan);

    // Input processing
    bool process_event_chunk(const ReportResultChunk &chunk);
    bool process_series_chunk(const ReportResultChunk &chunk);
    bool process_series_chunk(const ReportResultChunk &chunk,
                              size_t stream_index);
    bool process_edf_series_batch(size_t seed_chunk_index,
                                  uint32_t budget_ms,
                                  bool &processed);

    // Completion
    void finish();
    void fail(const char *message);

    // Runtime state
    ReportRangePlotRuntime &range_plot_;
    ReportResultCacheRuntime &cache_;
    ReportNightIndexService &night_index_;
    ReportEdfCatalogContext &edf_catalog_;
    ReportEdfPlotBatch edf_batch_;
};

}  // namespace aircannect
