#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "report_manager_internal_types.h"
#include "report_manager_limits.h"
#include "report_plot_build_phase.h"
#include "report_spool_types.h"

namespace aircannect {

class ReportPlotBuildState {
public:
    using PlotRange = report_manager_internal::PlotRange;
    using ReportResultChunk = report_manager_internal::ReportResultChunk;
    using PlotSeriesBuildState =
        report_manager_internal::PlotSeriesBuildState;

    void reset();
    uint32_t elapsed_ms(uint32_t now_ms) const;

    bool open_series(size_t stream_index, size_t stream_count);
    bool process_series_sample(size_t stream_index,
                               const ReportResultChunk &chunk,
                               const ReportSeriesSample &sample,
                               uint32_t interval_ms);
    bool append_decimated_series_gap(size_t stream_index);
    bool append_decimated_series_point(size_t stream_index,
                                       int64_t timestamp_ms,
                                       int32_t value_milli,
                                       int64_t bucket_ms,
                                       int64_t gap_threshold_ms);
    bool finish_series(size_t stream_index,
                       const ReportResultStream *streams,
                       size_t stream_count);

    ReportSpoolBuffer result_bin;
    ReportSpoolBuffer build_bin;
    ReportSpoolBuffer tmp;
    ReportSpoolBuffer seen_events;
    bool skip_cache = false;
    bool active_idle_prebuild = false;

    bool ok = true;
    bool active = false;
    bool idle_prebuild = false;
    std::atomic<uint64_t> night_start_ms{0};
    ReportPlotBuildPhase phase = ReportPlotBuildPhase::Idle;
    PlotRange ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    size_t range_count = 0;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int64_t bucket_ms = 1;
    uint32_t chunk_index = 0;
    bool chunk_done[AC_REPORT_RESULT_CHUNK_MAX] = {};
    PlotSeriesBuildState series_states[AC_REPORT_RESULT_STREAM_MAX];
    uint32_t started_ms = 0;
    uint32_t input_chunks = 0;
    uint32_t input_bytes = 0;
};

}  // namespace aircannect
