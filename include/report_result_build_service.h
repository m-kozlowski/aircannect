#pragma once

#include <stddef.h>
#include <stdint.h>

#include "large_text_buffer.h"
#include "report_result_cache_runtime.h"
#include "report_result_plot_builder.h"
#include "report_result_runtime.h"
#include "report_result_types.h"

namespace aircannect {

class ReportEdfCatalogContext;
struct ReportIndexedNight;
struct ReportSummaryRecord;

class ReportResultBuildService {
public:
    ReportResultBuildService(ReportResultCacheRuntime &cache,
                             ReportEdfCatalogContext &edf_catalog);

    // Materialized result state
    ReportResultRuntime &runtime() { return runtime_; }
    const ReportResultRuntime &runtime() const { return runtime_; }
    ReportResultStatus status() const;

    bool ensure_chunks();
    void clear_prepare();
    void defer_prepare(uint64_t night_start_ms,
                       size_t therapy_index,
                       const char *message);
    void fail_prepare(const char *message);
    void begin_prepare_for_night(size_t therapy_index,
                                 const ReportIndexedNight &night,
                                 const char *current_etag);
    bool resolve_and_materialize_for_night(const ReportIndexedNight &night,
                                           int64_t range_start_ms,
                                           int64_t range_end_ms,
                                           bool *edf_pending_out = nullptr);
    bool finalize_prepare(size_t therapy_index);
    void build_chunks_json(LargeTextBuffer &json,
                           size_t offset,
                           size_t limit) const;

    // Full-night plot build
    ReportResultPlotBuilder &plot_builder() { return plot_builder_; }
    const ReportResultPlotBuilder &plot_builder() const {
        return plot_builder_;
    }
    bool plot_active() const { return plot_builder_.active(); }

private:
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    bool count_events_from_chunks();
    void apply_event_indices_from_counts();
    bool apply_series_metrics_from_chunks();
    bool find_edf_sessions_for_night(const ReportSummaryRecord &night,
                                     int64_t range_start_ms,
                                     int64_t range_end_ms,
                                     bool *pending_out);

    ReportResultCacheRuntime &cache_;
    ReportEdfCatalogContext &edf_catalog_;
    ReportResultRuntime runtime_;
    ReportResultPlotBuilder plot_builder_;
};

}  // namespace aircannect
