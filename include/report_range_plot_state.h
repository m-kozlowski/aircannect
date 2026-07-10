#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_manager_internal_types.h"
#include "report_manager_limits.h"
#include "report_plot_build_phase.h"
#include "report_result_types.h"
#include "report_spool_types.h"

namespace aircannect {

class ReportRangePlotBuildState {
public:
    using PlotBuildBucket = report_manager_internal::PlotBuildBucket;
    using PlotRange = report_manager_internal::PlotRange;
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    ~ReportRangePlotBuildState();

    bool ensure_buffers();
    void reset();

    bool matches(size_t index,
                 uint64_t night_start_ms,
                 int64_t from_ms,
                 int64_t to_ms) const;

    bool open_series(size_t stream_index);
    bool process_series_sample(size_t stream_index,
                               const ReportSeriesSample &sample,
                               ReportSignalId signal,
                               ReportSourceId source,
                               uint32_t interval_ms,
                               int32_t scale,
                               bool &capped,
                               bool &overflow);
    bool append_decimated_series_gap(size_t stream_index);
    bool append_decimated_series_point(size_t stream_index,
                                       int64_t timestamp_ms,
                                       int32_t value_milli);
    bool finish_series(size_t stream_index, const char **error_out);

    ReportIndexedNight *indexed_night = nullptr;
    bool active = false;
    ReportPlotBuildPhase phase = ReportPlotBuildPhase::Idle;
    size_t index = 0;
    int64_t from_ms = 0;
    int64_t to_ms = 0;
    uint64_t night_start_ms = 0;

    ReportResultChunk *chunks = nullptr;
    size_t chunk_count = 0;
    ReportResultStream streams[AC_REPORT_RESULT_STREAM_MAX] = {};
    size_t stream_count = 0;
    EdfReportSessionDescriptor *edf_sessions = nullptr;
    size_t edf_session_count = 0;
    PlotRange *ranges = nullptr;
    size_t range_count = 0;

    std::shared_ptr<ReportSpoolBuffer> bytes;
    ReportSpoolBuffer tmp;
    ReportSpoolBuffer seen_events;
    uint32_t event_count = 0;
    uint32_t chunk_index = 0;
    size_t stream_index = 0;
    bool chunk_done[AC_REPORT_RESULT_CHUNK_MAX] = {};

    int64_t bucket_ms = 1;
    bool series_open[AC_REPORT_RESULT_STREAM_MAX] = {};
    uint32_t series_points[AC_REPORT_RESULT_STREAM_MAX] = {};
    bool have_last_sample[AC_REPORT_RESULT_STREAM_MAX] = {};
    int64_t last_sample_ms[AC_REPORT_RESULT_STREAM_MAX] = {};
    int last_range_index[AC_REPORT_RESULT_STREAM_MAX] = {};
    int64_t current_bucket[AC_REPORT_RESULT_STREAM_MAX] = {};
    PlotBuildBucket buckets[AC_REPORT_RESULT_STREAM_MAX] = {};
    ReportSpoolBuffer series_tmp[AC_REPORT_RESULT_STREAM_MAX];

    bool ok = true;
    uint32_t started_ms = 0;
    uint32_t input_chunks = 0;
    uint32_t input_bytes = 0;
};

}  // namespace aircannect
