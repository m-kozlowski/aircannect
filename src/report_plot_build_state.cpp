#include "report_plot_build_state.h"

#include <limits.h>
#include <string.h>

#include "board_report.h"
#include "report_plot_payload.h"

namespace aircannect {

void ReportPlotBuildState::reset() {
    active = false;
    idle_prebuild = false;
    night_start_ms.store(0);
    phase = ReportPlotBuildPhase::Idle;
    build_bin.clear();
    tmp.clear();
    ok = true;
    ranges = nullptr;
    range_count = 0;
    start_ms = 0;
    end_ms = 0;
    bucket_ms = 1;
    chunk_index = 0;
    memset(chunk_done, 0, sizeof(chunk_done));
    seen_events.clear();

    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        series_states[i].reset();
    }

    started_ms = 0;
    input_chunks = 0;
    input_bytes = 0;
}

uint32_t ReportPlotBuildState::elapsed_ms(uint32_t now_ms) const {
    return started_ms ? static_cast<uint32_t>(now_ms - started_ms) : 0;
}

bool ReportPlotBuildState::open_series(size_t stream_index,
                                       size_t stream_count) {
    if (stream_index >= stream_count ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
        return false;
    }

    PlotSeriesBuildState &state = series_states[stream_index];
    if (state.open) return true;

    state.reset();
    state.points.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    state.open = true;
    return ok;
}

bool ReportPlotBuildState::process_series_sample(
    size_t stream_index,
    const ReportResultChunk &chunk,
    const ReportSeriesSample &sample,
    uint32_t interval_ms) {
    if (stream_index >= AC_REPORT_RESULT_STREAM_MAX) return false;

    PlotSeriesBuildState &state = series_states[stream_index];

    int range_index = -1;
    if (state.last_range_index >= 0 &&
        static_cast<size_t>(state.last_range_index) < range_count) {
        const ReportSessionRange &last_range =
            ranges[state.last_range_index];
        if (sample.timestamp_ms >= last_range.start_ms &&
            sample.timestamp_ms < last_range.end_ms) {
            range_index = state.last_range_index;
        }
    }

    if (range_index < 0) {
        range_index =
            report_plot_range_index(ranges, range_count, sample.timestamp_ms);
    }

    if (range_index < 0) return true;

    if (state.have_last_sample &&
        (range_index != state.last_range_index ||
         sample.timestamp_ms >
             state.last_sample_ms + plot_gap_threshold_ms(interval_ms))) {
        if (!report_append_plot_series_gap(state, ok)) return false;
    }

    const int64_t sample_bucket_ms =
        plot_bucket_ms_for_signal(chunk.signal,
                                  chunk.source,
                                  bucket_ms,
                                  interval_ms,
                                  false);

    int32_t value_milli = sample.value_milli;
    if ((chunk.signal == ReportSignalId::Flow &&
         chunk.source == ReportSourceId::RespiratoryFlow6p25Hz) ||
        (chunk.signal == ReportSignalId::Leak &&
         chunk.source == ReportSourceId::Leak0p5Hz)) {
        const int64_t scaled = static_cast<int64_t>(value_milli) * 60LL;
        value_milli = scaled > INT32_MAX
                          ? INT32_MAX
                          : (scaled < INT32_MIN
                                 ? INT32_MIN
                                 : static_cast<int32_t>(scaled));
    }

    if (!report_append_plot_series_value(state,
                                         start_ms,
                                         sample.timestamp_ms,
                                         value_milli,
                                         sample_bucket_ms,
                                         ok)) {
        return false;
    }

    state.bucket.range_index = range_index;
    state.have_last_sample = true;
    state.last_sample_ms = sample.timestamp_ms;
    state.last_range_index = range_index;
    return true;
}

bool ReportPlotBuildState::append_decimated_series_gap(size_t stream_index) {
    if (stream_index >= AC_REPORT_RESULT_STREAM_MAX) return false;

    return report_append_plot_series_gap(series_states[stream_index], ok);
}

bool ReportPlotBuildState::append_decimated_series_point(
    size_t stream_index,
    int64_t timestamp_ms,
    int32_t value_milli,
    int64_t sample_bucket_ms,
    int64_t gap_threshold_ms) {
    if (stream_index >= AC_REPORT_RESULT_STREAM_MAX) return false;

    PlotSeriesBuildState &state = series_states[stream_index];
    if (state.have_last_sample &&
        timestamp_ms > state.last_sample_ms + gap_threshold_ms) {
        if (!report_append_plot_series_gap(state, ok)) return false;
    }

    return report_append_plot_series_point(state,
                                           start_ms,
                                           timestamp_ms,
                                           value_milli,
                                           sample_bucket_ms,
                                           ok);
}

bool ReportPlotBuildState::finish_series(size_t stream_index,
                                         const ReportResultStream *streams,
                                         size_t stream_count) {
    if (stream_index >= stream_count ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
        !streams) {
        return true;
    }

    const ReportResultStream &stream = streams[stream_index];
    PlotSeriesBuildState &state = series_states[stream_index];
    if (stream.kind != ReportStoreChunkKind::Series || !state.open) {
        state.reset();
        return true;
    }

    report_flush_plot_envelope_bucket(state, ok);
    if (state.points.size()) {
        const int64_t series_bucket_ms =
            state.series_bucket_ms > 0 ? state.series_bucket_ms : bucket_ms;

        if (!append_plot_series_envelope_runs(build_bin,
                                              stream.name,
                                              state.points,
                                              series_bucket_ms,
                                              ok)) {
            return false;
        }
    }

    state.reset();
    return true;
}

}  // namespace aircannect
