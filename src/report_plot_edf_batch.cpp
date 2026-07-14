#include "report_result_plot_builder.h"

#include <algorithm>

#include <Arduino.h>

#include "report_plot_payload.h"
#include "report_result_runtime.h"

namespace aircannect {

bool ReportResultPlotBuilder::process_edf_series_batch(
    size_t seed_chunk_index,
    uint32_t budget_ms,
    bool &processed) {
    processed = false;
    const uint32_t started_ms = millis();
    bool reader_started = false;
    if (!edf_batch_.active()) {
        ReportEdfPlotBatchInput input;
        input.chunks = result_.scratch().chunks();
        input.chunk_count = result_.status().chunk_count;
        input.streams = result_.streams().data();
        input.stream_count = result_.streams().count();
        input.edf_sessions = result_.scratch().edf_sessions();
        input.edf_session_count = result_.scratch().edf_session_count();
        input.ranges = result_.plot().ranges;
        input.range_count = result_.plot().range_count;
        input.chunk_done = result_.plot().chunk_done;
        input.input_chunks = &result_.plot().input_chunks;
        input.input_bytes = &result_.plot().input_bytes;
        input.window_start_ms = result_.plot().start_ms;
        input.window_end_ms = result_.plot().end_ms;
        input.plot_start_ms = result_.plot().start_ms;
        input.base_bucket_ms = result_.plot().bucket_ms;
        input.range_plot = false;

        ReportEdfPlotBatchSink sink;
        sink.context = this;
        sink.open_series = [](void *context, size_t stream_index) -> bool {
            ReportResultPlotBuilder *builder =
                static_cast<ReportResultPlotBuilder *>(context);
            return builder &&
                   builder->result_.plot().open_series(
                       stream_index, builder->result_.streams().count());
        };
        sink.append_point = [](
            void *context,
            size_t stream_index,
            const EdfReportSeriesPlotPoint &point,
            const EdfReportSeriesPlotConfig &config) -> bool {
            ReportResultPlotBuilder *builder =
                static_cast<ReportResultPlotBuilder *>(context);
            if (!builder) return false;
            if (point.gap) {
                return builder->result_.plot()
                    .append_decimated_series_gap(stream_index);
            }

            const int64_t gap_threshold_ms = std::max<int64_t>(
                static_cast<int64_t>(config.gap_threshold_ms),
                static_cast<int64_t>(config.bucket_ms) * 2LL);
            return builder->result_.plot().append_decimated_series_point(
                stream_index,
                point.timestamp_ms,
                point.value_milli,
                config.bucket_ms,
                gap_threshold_ms);
        };
        sink.append_sample = [](
            void *context,
            size_t stream_index,
            const ReportResultChunk &chunk,
            const ReportSeriesSample &sample) -> bool {
            ReportResultPlotBuilder *builder =
                static_cast<ReportResultPlotBuilder *>(context);
            if (!builder) return false;

            const uint32_t interval_ms = infer_chunk_interval_ms(
                chunk.record_count, chunk.start_ms, chunk.end_ms);
            return builder->result_.plot().process_series_sample(
                stream_index, chunk, sample, interval_ms);
        };

        if (!edf_batch_.start(seed_chunk_index, input, sink)) {
            fail(edf_batch_.error() ? edf_batch_.error()
                                    : "plot_series_decode_failed");
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
                                : "plot_series_decode_failed");
        return false;
    }

    processed = true;
    return result_.plot().ok;
}

}  // namespace aircannect
