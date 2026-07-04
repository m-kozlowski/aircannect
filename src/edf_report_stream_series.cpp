#include "edf_report_stream_series.h"

namespace aircannect {
namespace {

bool emit_stream_series_sample(EdfReportStreamSeriesContext &ctx,
                               const ReportSeriesSample &sample) {
    if (!ctx.callback) return false;
    if (!ctx.callback(ctx.context, sample)) return false;
    ctx.samples_emitted++;
    return true;
}

bool flush_stream_zero_run(EdfReportStreamSeriesContext &ctx) {
    if (!ctx.pending_zero) return true;
    if (ctx.interval_ms == 0) return false;

    for (uint32_t i = 0; i < ctx.pending_zero_count; ++i) {
        ReportSeriesSample zero;
        zero.timestamp_ms =
            ctx.pending_zero_start_ms +
            static_cast<int64_t>(i) * static_cast<int64_t>(ctx.interval_ms);
        zero.value_milli = 0;
        if (!emit_stream_series_sample(ctx, zero)) return false;
    }

    ctx.pending_zero = false;
    ctx.pending_zero_start_ms = 0;
    ctx.pending_zero_next_ms = 0;
    ctx.pending_zero_count = 0;
    return true;
}

}  // namespace

void edf_report_stream_series_clear_zero_run(
    EdfReportStreamSeriesContext &ctx) {
    if (!ctx.pending_zero) return;

    ctx.samples_trimmed += ctx.pending_zero_count;
    ctx.pending_zero = false;
    ctx.pending_zero_start_ms = 0;
    ctx.pending_zero_next_ms = 0;
    ctx.pending_zero_count = 0;
}

bool edf_report_stream_series_record_sample(
    void *context,
    const ReportSeriesSample &sample) {
    EdfReportStreamSeriesContext *ctx =
        static_cast<EdfReportStreamSeriesContext *>(context);
    if (!ctx || !ctx->callback) return false;

    const bool zero = sample.value_milli == 0;
    if (ctx->trim_leading && ctx->leading_open) {
        if (zero) {
            ctx->samples_trimmed++;
            return true;
        }
        ctx->leading_open = false;
    }

    if (ctx->trim_trailing && zero) {
        if (!ctx->pending_zero ||
            sample.timestamp_ms != ctx->pending_zero_next_ms) {
            if (!flush_stream_zero_run(*ctx)) return false;
            ctx->pending_zero = true;
            ctx->pending_zero_start_ms = sample.timestamp_ms;
            ctx->pending_zero_count = 0;
        }
        ctx->pending_zero_count++;
        ctx->pending_zero_next_ms =
            sample.timestamp_ms +
            static_cast<int64_t>(ctx->interval_ms);
        return true;
    }

    if (!flush_stream_zero_run(*ctx)) return false;
    return emit_stream_series_sample(*ctx, sample);
}

}  // namespace aircannect
