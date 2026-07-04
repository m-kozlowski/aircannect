#include "report_range_plot_builder.h"

#include <stdint.h>

#include <Arduino.h>

#include "debug_log.h"
#include "report_plot_payload.h"
#include "report_range_plot_runtime.h"
#include "report_result_cache_runtime.h"
#include "report_result_provider_bridge.h"

namespace aircannect {

bool ReportRangePlotBuilder::process_series_chunk(
    const ReportResultChunk &chunk) {
    return process_series_chunk(chunk, chunk.stream_index);
}

bool ReportRangePlotBuilder::process_series_chunk(
    const ReportResultChunk &chunk,
    size_t stream_index) {
    auto &state = range_plot_.state();

    if (stream_index >= state.stream_count) {
        fail("range_bad_stream");
        return false;
    }

    ReportProviderChunk provider_chunk;
    if (!report_provider_chunk_from_result_stream(chunk,
                                                  stream_index,
                                                  state.streams,
                                                  state.stream_count,
                                                  state.edf_sessions,
                                                  state.edf_session_count,
                                                  provider_chunk)) {
        fail("range_chunk_map_failed");
        return false;
    }
    const int32_t scale =
        plot_value_multiplier(provider_chunk.signal, provider_chunk.source);

    struct RangeSeriesContext {
        ReportRangePlotBuildState *state = nullptr;
        const ReportProviderSeriesReadStats *read_stats = nullptr;
        ReportSignalId signal = ReportSignalId::Flow;
        ReportSourceId source = ReportSourceId::Summary;
        size_t stream_index = 0;
        uint32_t interval_ms = 0;
        int32_t scale = 1;
        bool capped = false;
        bool overflow = false;
    };
    RangeSeriesContext ctx;
    ctx.state = &state;
    ctx.signal = provider_chunk.signal;
    ctx.source = provider_chunk.source;
    ctx.stream_index = stream_index;
    ctx.scale = scale;
    ctx.interval_ms =
        infer_chunk_interval_ms(provider_chunk.record_count,
                                provider_chunk.start_ms,
                                provider_chunk.end_ms);
    ReportProviderSeriesReadStats read_stats;
    ctx.read_stats = &read_stats;
    const bool ok = report_for_each_result_series_sample(
        chunk,
        stream_index,
        state.streams,
        state.stream_count,
        state.edf_sessions,
        state.edf_session_count,
        static_cast<int64_t>(state.night_start_ms),
        read_stats,
        [](void *context, const ReportSeriesSample &sample) -> bool {
            RangeSeriesContext *ctx =
                static_cast<RangeSeriesContext *>(context);
            ReportRangePlotBuildState *state = ctx ? ctx->state : nullptr;
            if (!state) return false;

            const uint32_t interval_ms =
                (ctx->read_stats && ctx->read_stats->interval_ms)
                    ? ctx->read_stats->interval_ms
                    : ctx->interval_ms;

            return state->process_series_sample(ctx->stream_index,
                                                sample,
                                                ctx->signal,
                                                ctx->source,
                                                interval_ms,
                                                ctx->scale,
                                                ctx->capped,
                                                ctx->overflow);
        },
        &ctx);
    if (!ok && !ctx.capped) {
        fail(ctx.overflow ? "range_overflow"
                          : "range_series_decode_failed");
        return false;
    }
    (void)read_stats;
    return true;
}

void ReportRangePlotBuilder::finish() {
    if (!range_plot_.state().bytes) {
        fail("range_bad_state");
        return;
    }
    const PlotBlobScan scan = scan_plot_blob(*range_plot_.state().bytes);
    if (!scan.valid) {
        fail("range_invalid_blob");
        return;
    }

    cache_.finish_range_request(range_plot_.state().index,
                                range_plot_.state().night_start_ms,
                                range_plot_.state().from_ms,
                                range_plot_.state().to_ms,
                                range_plot_.state().bytes);
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Range plot ready index=%lu points=%lu input_chunks=%lu "
              "input_bytes=%lu bytes=%lu elapsed_ms=%lu\n",
              static_cast<unsigned long>(range_plot_.state().index),
              static_cast<unsigned long>(scan.points),
              static_cast<unsigned long>(range_plot_.state().input_chunks),
              static_cast<unsigned long>(range_plot_.state().input_bytes),
              static_cast<unsigned long>(range_plot_.state().bytes->size()),
              static_cast<unsigned long>(
                  range_plot_.state().started_ms
                      ? static_cast<uint32_t>(millis() -
                                              range_plot_.state().started_ms)
                      : 0));
    reset(false);
}

void ReportRangePlotBuilder::fail(const char *message) {
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "Range plot failed index=%lu error=%s\n",
              static_cast<unsigned long>(range_plot_.state().index),
              message ? message : "range_failed");
    cache_.fail_range_request(range_plot_.state().index,
                              range_plot_.state().night_start_ms,
                              range_plot_.state().from_ms,
                              range_plot_.state().to_ms);
    reset(false);
}

}  // namespace aircannect
