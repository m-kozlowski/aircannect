#pragma once

#include <stddef.h>

#include "report_manager_internal_types.h"
#include "report_night_index.h"
#include "report_plot_build_state.h"
#include "report_result_identity.h"
#include "report_result_range_set.h"
#include "report_result_scratch.h"
#include "report_result_stream_set.h"
#include "report_result_types.h"

namespace aircannect {

class ReportResultCacheRuntime;

class ReportResultRuntime {
public:
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    ReportResultScratch &scratch() { return scratch_; }
    const ReportResultScratch &scratch() const { return scratch_; }

    ReportResultStreamSet &streams() { return streams_; }
    const ReportResultStreamSet &streams() const { return streams_; }

    ReportResultIdentity &identity() { return identity_; }
    const ReportResultIdentity &identity() const { return identity_; }

    ReportResultRangeSet &ranges() { return ranges_; }
    const ReportResultRangeSet &ranges() const { return ranges_; }

    ReportResultStatus &status() { return status_; }
    const ReportResultStatus &status() const { return status_; }

    ReportPlotBuildState &plot() { return plot_; }
    const ReportPlotBuildState &plot() const { return plot_; }

    ReportResultStatus status_with_diagnostics(
        const ReportResultCacheRuntime &cache) const;

    bool ensure_chunks();
    bool ensure_edf_sessions();
    bool ensure_resolve_buffers();
    bool uses_edf_provider() const;
    void release_edf_sessions();

    bool set_ranges_from_indexed_night(const ReportIndexedNight &night);
    bool data_span(int64_t &span_start_ms, int64_t &span_end_ms) const;

    void clear_prepare_state();
    void mark_error(const char *message);
    const char *state_name() const;
    bool current_result_publishable(size_t therapy_index,
                                    const ReportIndexedNight &night,
                                    const char *etag,
                                    bool refresh_cache) const;

    bool add_stream(ReportStoreChunkKind kind,
                    ReportSourceId source,
                    ReportSignalId signal,
                    const char *name,
                    bool required,
                    bool complete,
                    size_t &stream_index);

private:
    ReportResultScratch scratch_;
    ReportResultStreamSet streams_;
    ReportResultIdentity identity_;
    ReportResultRangeSet ranges_;
    ReportResultStatus status_;
    ReportPlotBuildState plot_;
};

}  // namespace aircannect
