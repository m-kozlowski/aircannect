#include "edf_report_uniform_series.h"

#include <limits.h>

#include "memory_manager.h"

namespace aircannect {
namespace {

void clear_missing_bit(uint8_t *bits, size_t bytes, uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bits || byte >= bytes) return;
    bits[byte] &= static_cast<uint8_t>(~(1u << (index % 8u)));
}

void set_missing_bit(uint8_t *bits, size_t bytes, uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bits || byte >= bytes) return;
    bits[byte] |= static_cast<uint8_t>(1u << (index % 8u));
}

bool missing_bit_set(const uint8_t *bits, size_t bytes, uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bits || byte >= bytes) return true;
    return (bits[byte] & static_cast<uint8_t>(1u << (index % 8u))) != 0;
}

}  // namespace

void EdfReportUniformSeriesData::clear() {
    if (missing_bitmap) {
        Memory::free(missing_bitmap);
        missing_bitmap = nullptr;
    }
    if (values) {
        Memory::free(values);
        values = nullptr;
    }

    interval_ms = 0;
    sample_count = 0;
    missing_bitmap_bytes = 0;
}

uint32_t edf_report_uniform_series_trim_edge_zero_padding(
    EdfReportUniformSeriesBuildContext &ctx,
    bool trim_leading,
    bool trim_trailing) {
    if (!ctx.values || !ctx.missing_bitmap || ctx.sample_count == 0) {
        return 0;
    }

    uint32_t trimmed = 0;
    if (trim_leading) {
        for (uint32_t i = 0; i < ctx.sample_count; ++i) {
            if (missing_bit_set(ctx.missing_bitmap,
                                ctx.missing_bitmap_bytes,
                                i)) {
                continue;
            }
            if (ctx.values[i] != 0) break;
            set_missing_bit(ctx.missing_bitmap, ctx.missing_bitmap_bytes, i);
            trimmed++;
        }
    }

    if (trim_trailing) {
        for (uint32_t i = ctx.sample_count; i > 0; --i) {
            const uint32_t index = i - 1;
            if (missing_bit_set(ctx.missing_bitmap,
                                ctx.missing_bitmap_bytes,
                                index)) {
                continue;
            }
            if (ctx.values[index] != 0) break;
            set_missing_bit(ctx.missing_bitmap,
                            ctx.missing_bitmap_bytes,
                            index);
            trimmed++;
        }
    }

    return trimmed;
}

bool edf_report_uniform_series_record_sample(
    void *context,
    const ReportSeriesSample &sample) {
    EdfReportUniformSeriesBuildContext *ctx =
        static_cast<EdfReportUniformSeriesBuildContext *>(context);
    if (!ctx || !ctx->values || !ctx->missing_bitmap ||
        ctx->interval_ms == 0 || sample.timestamp_ms < ctx->start_ms) {
        return false;
    }

    const int64_t delta = sample.timestamp_ms - ctx->start_ms;
    if (delta < 0 || delta % static_cast<int64_t>(ctx->interval_ms) != 0) {
        ctx->bad_sample = true;
        return false;
    }

    const int64_t index64 = delta / static_cast<int64_t>(ctx->interval_ms);
    if (index64 < 0 || index64 > UINT32_MAX ||
        static_cast<uint32_t>(index64) >= ctx->sample_count) {
        ctx->bad_sample = true;
        return false;
    }

    const uint32_t index = static_cast<uint32_t>(index64);
    ctx->values[index] = sample.value_milli;
    clear_missing_bit(ctx->missing_bitmap,
                      ctx->missing_bitmap_bytes,
                      index);
    return true;
}

}  // namespace aircannect
