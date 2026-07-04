#include "report_result_plot_builder.h"

#include <stdint.h>

#include "report_data_provider.h"
#include "report_plot_payload.h"
#include "report_records.h"
#include "report_result_provider_bridge.h"
#include "report_result_runtime.h"

namespace aircannect {

bool ReportResultPlotBuilder::process_series_chunk(size_t chunk_index) {
    if (chunk_index >= result_.status().chunk_count ||
        chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail("plot_bad_chunk");
        return false;
    }

    const ReportResultChunk &chunk = result_.scratch().chunks()[chunk_index];
    if (chunk.stream_index >= result_.streams().count() ||
        chunk.stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
        !result_.plot().open_series(chunk.stream_index,
                                    result_.streams().count())) {
        fail("plot_series_open_failed");
        return false;
    }

    struct PlotSeriesContext {
        ReportPlotBuildState *plot = nullptr;
        const ReportResultChunk *chunk = nullptr;
        const ReportProviderSeriesReadStats *read_stats = nullptr;
        uint32_t interval_ms = 0;
    };

    PlotSeriesContext ctx;
    ctx.plot = &result_.plot();
    ctx.chunk = &chunk;
    ctx.interval_ms =
        infer_chunk_interval_ms(chunk.record_count, chunk.start_ms, chunk.end_ms);

    ReportProviderSeriesReadStats read_stats;
    ctx.read_stats = &read_stats;

    const bool ok = report_for_each_result_series_sample(
        chunk,
        chunk.stream_index,
        result_.streams().data(),
        result_.streams().count(),
        result_.scratch().edf_sessions(),
        result_.scratch().edf_session_count(),
        static_cast<int64_t>(result_.identity().summary().start_ms),
        read_stats,
        [](void *context, const ReportSeriesSample &sample) -> bool {
            PlotSeriesContext *ctx =
                static_cast<PlotSeriesContext *>(context);
            ReportPlotBuildState *plot = ctx ? ctx->plot : nullptr;
            const ReportResultChunk *chunk = ctx ? ctx->chunk : nullptr;
            if (!plot || !chunk) return false;

            const uint32_t interval_ms =
                (ctx->read_stats && ctx->read_stats->interval_ms)
                    ? ctx->read_stats->interval_ms
                    : ctx->interval_ms;

            return plot->process_series_sample(chunk->stream_index,
                                               *chunk,
                                               sample,
                                               interval_ms);
        },
        &ctx);
    if (!ok) {
        fail("plot_series_decode_failed");
        return false;
    }

    (void)read_stats;
    return true;
}

}  // namespace aircannect
