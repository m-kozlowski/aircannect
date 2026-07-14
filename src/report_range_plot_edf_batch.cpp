#include "report_range_plot_builder.h"

#include <Arduino.h>

#include "report_plot_payload.h"
#include "report_range_plot_runtime.h"

namespace aircannect {

bool ReportRangePlotBuilder::process_edf_series_batch(
    size_t seed_chunk_index,
    uint32_t budget_ms,
    bool &processed) {
    processed = false;
    const uint32_t started_ms = millis();
    bool reader_started = false;
    auto &state = range_plot_.state();
    if (!edf_batch_.active()) {
        ReportEdfPlotBatchInput input;
        input.chunks = state.chunks;
        input.chunk_count = state.chunk_count;
        input.streams = state.streams;
        input.stream_count = state.stream_count;
        input.edf_sessions = state.edf_sessions;
        input.edf_session_count = state.edf_session_count;
        input.ranges = state.ranges;
        input.range_count = state.range_count;
        input.chunk_done = state.chunk_done;
        input.input_chunks = &state.input_chunks;
        input.input_bytes = &state.input_bytes;
        input.window_start_ms = state.from_ms;
        input.window_end_ms = state.to_ms;
        input.plot_start_ms = state.from_ms;
        input.base_bucket_ms = state.bucket_ms;
        input.range_plot = true;

        ReportEdfPlotBatchSink sink;
        sink.context = this;
        sink.open_series = [](void *context, size_t stream_index) -> bool {
            ReportRangePlotBuilder *builder =
                static_cast<ReportRangePlotBuilder *>(context);
            return builder &&
                   builder->range_plot_.state().open_series(stream_index);
        };
        sink.append_point = [](
            void *context,
            size_t stream_index,
            const EdfReportSeriesPlotPoint &point,
            const EdfReportSeriesPlotConfig &) -> bool {
            ReportRangePlotBuilder *builder =
                static_cast<ReportRangePlotBuilder *>(context);
            if (!builder) return false;

            auto &state = builder->range_plot_.state();
            if (point.gap) {
                return state.append_decimated_series_gap(stream_index);
            }
            return state.append_decimated_series_point(stream_index,
                                                       point.timestamp_ms,
                                                       point.value_milli);
        };
        sink.append_sample = [](
            void *context,
            size_t stream_index,
            const ReportResultChunk &chunk,
            const ReportSeriesSample &sample) -> bool {
            ReportRangePlotBuilder *builder =
                static_cast<ReportRangePlotBuilder *>(context);
            if (!builder) return false;

            const uint32_t interval_ms = infer_chunk_interval_ms(
                chunk.record_count, chunk.start_ms, chunk.end_ms);
            const int32_t scale =
                plot_value_multiplier(chunk.signal, chunk.source);
            bool capped = false;
            bool overflow = false;
            return builder->range_plot_.state().process_series_sample(
                stream_index,
                sample,
                chunk.signal,
                chunk.source,
                interval_ms,
                scale,
                capped,
                overflow);
        };

        if (!edf_batch_.start(seed_chunk_index, input, sink)) {
            fail(edf_batch_.error() ? edf_batch_.error()
                                    : "range_series_decode_failed");
            return false;
        }
        reader_started = true;
    }

    const uint32_t setup_elapsed_ms =
        static_cast<uint32_t>(millis() - started_ms);
    if (reader_started && setup_elapsed_ms >= budget_ms) return true;

    const uint32_t remaining_budget_ms =
        setup_elapsed_ms < budget_ms ? budget_ms - setup_elapsed_ms : 0;
    const ReportEdfPlotBatchResult result =
        edf_batch_.poll(remaining_budget_ms);
    if (result == ReportEdfPlotBatchResult::Pending) return true;
    if (result == ReportEdfPlotBatchResult::Failed) {
        fail(edf_batch_.error() ? edf_batch_.error()
                                : "range_series_decode_failed");
        return false;
    }

    processed = true;
    return state.ok;
}

}  // namespace aircannect
