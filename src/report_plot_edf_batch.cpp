#include "report_result_plot_builder.h"

#include <algorithm>
#include <stdint.h>

#include "board.h"
#include "edf_report_data_plan.h"
#include "edf_report_data_reader.h"
#include "edf_report_provider.h"
#include "edf_report_provider_token.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_plot_payload.h"
#include "report_records.h"
#include "report_result_provider_bridge.h"
#include "report_result_runtime.h"
#include "report_source_resolver.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

const EdfReportProvider &edf_report_provider() {
    static EdfReportProvider provider;
    return provider;
}

}  // namespace

bool ReportResultPlotBuilder::process_edf_series_batch(size_t seed_chunk_index,
                                                       bool &processed) {
    processed = false;
    if (!result_.scratch().chunks() ||
        seed_chunk_index >= result_.status().chunk_count ||
        seed_chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail("plot_bad_chunk");
        return false;
    }

    const ReportResultChunk &seed = result_.scratch().chunks()[seed_chunk_index];
    if (seed.kind != ReportStoreChunkKind::Series ||
        seed.provider_ref.provider != ReportProviderId::Edf ||
        result_.plot().chunk_done[seed_chunk_index]) {
        return true;
    }

    const size_t max_chunks =
        std::min(static_cast<size_t>(result_.status().chunk_count),
                 static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));
    if (max_chunks == 0) return true;

    const size_t candidate_capacity =
        max_chunks *
        std::min(result_.streams().count(),
                 static_cast<size_t>(AC_REPORT_RESULT_STREAM_MAX));
    if (candidate_capacity == 0) return true;

    ReportProviderChunk *candidates =
        static_cast<ReportProviderChunk *>(Memory::calloc_large(
            candidate_capacity, sizeof(ReportProviderChunk), false));
    ReportResultChunk *logical_chunks =
        static_cast<ReportResultChunk *>(Memory::calloc_large(
            candidate_capacity, sizeof(ReportResultChunk), false));
    size_t *chunk_indices = static_cast<size_t *>(Memory::calloc_large(
        candidate_capacity, sizeof(size_t), false));
    uint8_t *stream_indices = static_cast<uint8_t *>(Memory::calloc_large(
        candidate_capacity, sizeof(uint8_t), false));
    bool *selected = static_cast<bool *>(Memory::calloc_large(
        candidate_capacity, sizeof(bool), false));
    bool *physical_counted = static_cast<bool *>(Memory::calloc_large(
        max_chunks, sizeof(bool), false));
    ReportProviderSeriesReadStats *stats =
        static_cast<ReportProviderSeriesReadStats *>(Memory::calloc_large(
            candidate_capacity, sizeof(ReportProviderSeriesReadStats), false));
    EdfReportSeriesPlotConfig *plot_configs =
        static_cast<EdfReportSeriesPlotConfig *>(Memory::calloc_large(
            candidate_capacity, sizeof(EdfReportSeriesPlotConfig), false));
    if (!candidates || !logical_chunks || !chunk_indices ||
        !stream_indices || !selected || !physical_counted || !stats ||
        !plot_configs) {
        if (candidates) Memory::free(candidates);
        if (logical_chunks) Memory::free(logical_chunks);
        if (chunk_indices) Memory::free(chunk_indices);
        if (stream_indices) Memory::free(stream_indices);
        if (selected) Memory::free(selected);
        if (physical_counted) Memory::free(physical_counted);
        if (stats) Memory::free(stats);
        if (plot_configs) Memory::free(plot_configs);
        fail("plot_alloc_failed");
        return false;
    }

    EdfReportPlotRange fast_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    const size_t fast_range_count =
        std::min(result_.plot().range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < fast_range_count; ++i) {
        fast_ranges[i].start_ms = result_.plot().ranges[i].start_ms;
        fast_ranges[i].end_ms = result_.plot().ranges[i].end_ms;
    }

    size_t candidate_count = 0;
    auto add_candidate = [&](size_t index, size_t stream_index) {
        const ReportResultChunk &chunk = result_.scratch().chunks()[index];
        if (result_.plot().chunk_done[index] ||
            chunk.kind != ReportStoreChunkKind::Series ||
            chunk.provider_ref.provider != ReportProviderId::Edf ||
            stream_index >= result_.streams().count() ||
            stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
            !report_result_chunk_has_stream(chunk, stream_index) ||
            candidate_count >= candidate_capacity) {
            return;
        }

        if (!report_provider_chunk_from_result_stream(
                chunk,
                stream_index,
                result_.streams().data(),
                result_.streams().count(),
                result_.scratch().edf_sessions(),
                result_.scratch().edf_session_count(),
                candidates[candidate_count])) {
            return;
        }

        ReportResultChunk &logical = logical_chunks[candidate_count];
        logical = chunk;
        logical.provider_ref = candidates[candidate_count].ref;
        logical.source = candidates[candidate_count].source;
        logical.signal = candidates[candidate_count].signal;
        logical.name = candidates[candidate_count].name;
        logical.stream_index = static_cast<uint8_t>(stream_index);
        logical.stream_mask = 0;
        logical.start_ms = candidates[candidate_count].start_ms;
        logical.end_ms = candidates[candidate_count].end_ms;
        logical.record_count = candidates[candidate_count].record_count;
        logical.payload_len = candidates[candidate_count].payload_len;
        logical.payload_schema = candidates[candidate_count].payload_schema;

        const uint32_t interval_ms = infer_chunk_interval_ms(
            logical.record_count, logical.start_ms, logical.end_ms);
        plot_configs[candidate_count].ranges = fast_ranges;
        plot_configs[candidate_count].range_count = fast_range_count;
        plot_configs[candidate_count].plot_start_ms = result_.plot().start_ms;
        plot_configs[candidate_count].bucket_ms =
            static_cast<uint32_t>(std::min<int64_t>(
                UINT32_MAX,
                plot_bucket_ms_for_signal(logical.signal,
                                          logical.source,
                                          result_.plot().bucket_ms,
                                          interval_ms,
                                          false)));
        plot_configs[candidate_count].gap_threshold_ms =
            static_cast<uint32_t>(std::min<int64_t>(
                UINT32_MAX, plot_gap_threshold_ms(interval_ms)));
        plot_configs[candidate_count].value_multiplier =
            plot_value_multiplier(logical.signal, logical.source);
        chunk_indices[candidate_count] = index;
        stream_indices[candidate_count] = static_cast<uint8_t>(stream_index);
        ++candidate_count;
    };

    for (size_t stream_index = 0;
         stream_index < result_.streams().count() &&
         stream_index < AC_REPORT_RESULT_STREAM_MAX;
         ++stream_index) {
        add_candidate(seed_chunk_index, stream_index);
    }
    for (size_t i = 0; i < max_chunks; ++i) {
        if (i == seed_chunk_index) continue;

        for (size_t stream_index = 0;
             stream_index < result_.streams().count() &&
             stream_index < AC_REPORT_RESULT_STREAM_MAX;
             ++stream_index) {
            add_candidate(i, stream_index);
        }
    }
    if (candidate_count == 0) {
        Memory::free(plot_configs);
        Memory::free(stats);
        Memory::free(physical_counted);
        Memory::free(selected);
        Memory::free(stream_indices);
        Memory::free(chunk_indices);
        Memory::free(logical_chunks);
        Memory::free(candidates);
        return true;
    }

    struct EdfBatchContext {
        ReportResultPlotBuilder *builder = nullptr;
        const size_t *chunk_indices = nullptr;
        const uint8_t *stream_indices = nullptr;
        const ReportResultChunk *logical_chunks = nullptr;
        const EdfReportSeriesPlotConfig *plot_configs = nullptr;
        size_t chunk_count = 0;
        uint32_t points = 0;
    };

    EdfBatchContext ctx;
    ctx.builder = this;
    ctx.chunk_indices = chunk_indices;
    ctx.stream_indices = stream_indices;
    ctx.logical_chunks = logical_chunks;
    ctx.plot_configs = plot_configs;
    ctx.chunk_count = candidate_count;

    bool ok = false;
    if (!edf_report_signal_uses_edge_zero_padding(seed.signal)) {
        ok = edf_report_provider().for_each_compatible_series_plot_batch(
            candidates,
            candidate_count,
            plot_configs,
            selected,
            result_.scratch().edf_sessions(),
            result_.scratch().edf_session_count(),
            stats,
            [](void *context,
               size_t candidate_index,
               const EdfReportSeriesPlotPoint &point) -> bool {
                EdfBatchContext *ctx =
                    static_cast<EdfBatchContext *>(context);
                ReportResultPlotBuilder *builder = ctx ? ctx->builder : nullptr;
                if (!builder || !ctx->chunk_indices ||
                    !ctx->stream_indices || !ctx->logical_chunks ||
                    !ctx->plot_configs ||
                    candidate_index >= ctx->chunk_count) {
                    return false;
                }

                const size_t chunk_index =
                    ctx->chunk_indices[candidate_index];
                if (chunk_index >= builder->result_.status().chunk_count ||
                    chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
                    return false;
                }

                const size_t stream_index =
                    ctx->stream_indices[candidate_index];
                if (stream_index >= builder->result_.streams().count() ||
                    stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
                    !builder->result_.plot().open_series(
                        stream_index,
                        builder->result_.streams().count())) {
                    return false;
                }

                if (point.gap) {
                    ctx->points++;
                    return builder->result_.plot()
                        .append_decimated_series_gap(stream_index);
                }

                const EdfReportSeriesPlotConfig &config =
                    ctx->plot_configs[candidate_index];
                const int64_t decimated_gap_threshold_ms =
                    std::max<int64_t>(
                        static_cast<int64_t>(config.gap_threshold_ms),
                        static_cast<int64_t>(config.bucket_ms) * 2LL);

                ctx->points++;
                return builder->result_.plot()
                    .append_decimated_series_point(stream_index,
                                                   point.timestamp_ms,
                                                   point.value_milli,
                                                   config.bucket_ms,
                                                   decimated_gap_threshold_ms);
            },
            &ctx);
    }

    if (ok) {
        for (size_t i = 0; i < candidate_count; ++i) {
            if (!selected[i]) continue;

            const size_t chunk_index = chunk_indices[i];
            result_.plot().chunk_done[chunk_index] = true;
            if (chunk_index < max_chunks && !physical_counted[chunk_index]) {
                physical_counted[chunk_index] = true;
                result_.plot().input_chunks++;
                result_.plot().input_bytes +=
                    result_.scratch().chunks()[chunk_index].payload_len;
            }
            processed = true;
        }
    } else if (ctx.points == 0 && result_.plot().ok) {
        ok = edf_report_provider().for_each_compatible_series_sample_batch(
            candidates,
            candidate_count,
            selected,
            result_.scratch().edf_sessions(),
            result_.scratch().edf_session_count(),
            stats,
            [](void *context,
               size_t candidate_index,
               const ReportSeriesSample &sample) -> bool {
                EdfBatchContext *ctx = static_cast<EdfBatchContext *>(context);
                ReportResultPlotBuilder *builder = ctx ? ctx->builder : nullptr;
                if (!builder || !ctx->chunk_indices ||
                    candidate_index >= ctx->chunk_count) {
                    return false;
                }

                const size_t chunk_index = ctx->chunk_indices[candidate_index];
                if (chunk_index >= builder->result_.status().chunk_count ||
                    chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
                    return false;
                }

                const size_t stream_index =
                    ctx->stream_indices[candidate_index];
                const ReportResultChunk &logical =
                    ctx->logical_chunks[candidate_index];
                if (stream_index >= builder->result_.streams().count() ||
                    stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
                    !builder->result_.plot().open_series(
                        stream_index,
                        builder->result_.streams().count())) {
                    return false;
                }

                const uint32_t interval_ms = infer_chunk_interval_ms(
                    logical.record_count, logical.start_ms, logical.end_ms);
                return builder->result_.plot().process_series_sample(
                    stream_index,
                    logical,
                    sample,
                    interval_ms);
            },
            &ctx);

        if (ok) {
            for (size_t i = 0; i < candidate_count; ++i) {
                if (!selected[i]) continue;

                const size_t chunk_index = chunk_indices[i];
                result_.plot().chunk_done[chunk_index] = true;
                if (chunk_index < max_chunks &&
                    !physical_counted[chunk_index]) {
                    physical_counted[chunk_index] = true;
                    result_.plot().input_chunks++;
                    result_.plot().input_bytes +=
                        result_.scratch().chunks()[chunk_index].payload_len;
                }
                processed = true;
            }
        }
    }

    Memory::free(plot_configs);
    Memory::free(stats);
    Memory::free(physical_counted);
    Memory::free(selected);
    Memory::free(stream_indices);
    Memory::free(chunk_indices);
    Memory::free(logical_chunks);
    Memory::free(candidates);

    if (!ok || !processed || !result_.plot().ok) {
        fail(ok && processed ? "plot_overflow"
                             : "plot_series_decode_failed");
        return false;
    }
    return true;
}

}  // namespace aircannect
