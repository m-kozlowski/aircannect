#include "report_range_plot_builder.h"

#include <algorithm>
#include <stdint.h>
#include <string.h>

#include "board_report.h"
#include "edf_report_data_plan.h"
#include "edf_report_provider.h"
#include "report_data_provider.h"
#include "report_event_dedupe.h"
#include "report_manager_helpers.h"
#include "report_plot_payload.h"
#include "report_range_plot_runtime.h"
#include "report_records.h"
#include "report_result_provider_bridge.h"
#include "report_source_resolver.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

struct RangeChunkContext {
    ReportRangePlotBuilder *builder = nullptr;
    size_t stream_index = SIZE_MAX;
    const char *name = nullptr;
};

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

}  // namespace

bool ReportRangePlotBuilder::process_event_chunk(
    const ReportResultChunk &chunk) {
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!report_read_result_chunk_payload(
            chunk,
            static_cast<int64_t>(range_plot_.state().night_start_ms),
            range_plot_.state().edf_sessions,
            range_plot_.state().edf_session_count,
            meta,
            payload)) {
        fail("range_event_read_failed");
        return false;
    }
    const size_t wire = report_event_record_wire_size();
    const size_t count = wire ? payload.size() / wire : 0;
    for (size_t index = 0; index < count; ++index) {
        ReportEventRecord event;
        if (!report_read_event_record(payload.data(),
                                      payload.size(),
                                      index,
                                      event)) {
            continue;
        }
        if (!report_event_overlaps_window(event,
                                          range_plot_.state().from_ms,
                                          range_plot_.state().to_ms)) {
            continue;
        }

        bool in_session_range = false;
        for (size_t i = 0; i < range_plot_.state().range_count; ++i) {
            if (report_event_overlaps_window(
                    event,
                    range_plot_.state().ranges[i].start_ms,
                    range_plot_.state().ranges[i].end_ms,
                    AC_REPORT_EVENT_EDGE_TOLERANCE_MS)) {
                in_session_range = true;
                break;
            }
        }
        if (!in_session_range) continue;

        if (report_event_seen(range_plot_.state().seen_events, event)) continue;
        if (!remember_report_event(range_plot_.state().seen_events, event)) {
            fail("range_event_dedupe_failed");
            return false;
        }
        range_plot_.state().ok &=
            bin_put_i32(range_plot_.state().tmp,
                        static_cast<int32_t>(event.start_ms -
                                             range_plot_.state().from_ms));
        range_plot_.state().ok &=
            bin_put_i32(range_plot_.state().tmp,
                        static_cast<int32_t>(event.duration_ms));
        range_plot_.state().ok &=
            bin_put_i32(range_plot_.state().tmp,
                        static_cast<int32_t>(event.code));
        range_plot_.state().ok &=
            bin_put_i32(range_plot_.state().tmp,
                        static_cast<int32_t>(event.flags));
        if (!range_plot_.state().ok) {
            fail("range_overflow");
            return false;
        }
        ++range_plot_.state().event_count;
    }
    return true;
}

bool ReportRangePlotBuilder::collect_chunk(void *context,
                                           const ReportProviderChunk &info) {
    RangeChunkContext *ctx = static_cast<RangeChunkContext *>(context);
    if (!ctx || !ctx->builder || !info.name || !info.name[0]) return false;
    if (ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) != 0) {
        return true;
    }
    return ctx->builder->add_provider_chunk(info, ctx->stream_index);
}

bool ReportRangePlotBuilder::add_provider_chunk(
    const ReportProviderChunk &provider_chunk,
    size_t stream_index) {
    if (stream_index >= range_plot_.state().stream_count ||
        stream_index > UINT8_MAX) {
        fail("range_bad_stream");
        return false;
    }
    uint32_t stream_bit = 0;
    if (!report_manager_internal::report_stream_bit(stream_index, stream_bit)) {
        fail("range_bad_stream");
        return false;
    }
    if (!range_plot_.state().chunks) {
        fail("range_chunks_missing");
        return false;
    }

    ReportResultStream &stream = range_plot_.state().streams[stream_index];
    if (stream.kind != provider_chunk.kind ||
        stream.signal != provider_chunk.signal ||
        !stream.name ||
        strcmp(stream.name, provider_chunk.name) != 0) {
        fail("range_stream_mismatch");
        return false;
    }
    auto account_stream = [&]() {
        stream.chunk_count++;
        stream.record_count += provider_chunk.record_count;
        stream.payload_bytes += provider_chunk.payload_len;
        stream.has_edf_segment =
            stream.has_edf_segment ||
            provider_chunk.ref.provider == ReportProviderId::Edf;
        stream.has_spool_segment =
            stream.has_spool_segment ||
            provider_chunk.ref.provider == ReportProviderId::Spool;
    };

    for (size_t i = 0; i < range_plot_.state().chunk_count; ++i) {
        ReportResultChunk &existing = range_plot_.state().chunks[i];
        const bool same_physical =
            report_result_chunk_same_physical_edf(existing, provider_chunk);
        const bool same_logical =
            existing.kind == provider_chunk.kind &&
            existing.source == provider_chunk.source &&
            existing.name && provider_chunk.name &&
            strcmp(existing.name, provider_chunk.name) == 0 &&
            existing.start_ms == provider_chunk.start_ms &&
            existing.end_ms == provider_chunk.end_ms &&
            report_provider_chunk_ref_equal(existing.provider_ref,
                                            provider_chunk.ref);
        if (!same_physical && !same_logical) continue;
        if ((existing.stream_mask & stream_bit) != 0) return true;
        existing.stream_mask |= stream_bit;
        account_stream();
        return true;
    }

    if (range_plot_.state().chunk_count >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail("range_chunks_full");
        return false;
    }

    ReportResultChunk &chunk =
        range_plot_.state().chunks[range_plot_.state().chunk_count++];
    chunk.provider_ref = provider_chunk.ref;
    chunk.kind = provider_chunk.kind;
    chunk.source = provider_chunk.source;
    chunk.signal = provider_chunk.signal;
    chunk.name = provider_chunk.name;
    chunk.stream_index = static_cast<uint8_t>(stream_index);
    chunk.stream_mask = stream_bit;
    chunk.start_ms = provider_chunk.start_ms;
    chunk.end_ms = provider_chunk.end_ms;
    chunk.payload_schema = provider_chunk.payload_schema;
    chunk.record_count = provider_chunk.record_count;
    chunk.payload_len = provider_chunk.payload_len;

    account_stream();
    return true;
}

bool ReportRangePlotBuilder::materialize_plan(
    const ReportIndexedNight &night,
    const ReportResolvedPlan &plan) {
    range_plot_.state().night_start_ms = night.summary.start_ms;
    range_plot_.state().chunk_count = 0;
    for (size_t i = 0; i < AC_REPORT_RESULT_CHUNK_MAX; ++i) {
        range_plot_.state().chunks[i] = ReportResultChunk{};
    }
    range_plot_.state().stream_count =
        std::min(plan.stream_count,
                 static_cast<size_t>(AC_REPORT_RESULT_STREAM_MAX));
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        range_plot_.state().streams[i] = ReportResultStream{};
    }
    for (size_t i = 0; i < range_plot_.state().stream_count; ++i) {
        const ReportResolvedStream &resolved = plan.streams[i];
        ReportResultStream &stream = range_plot_.state().streams[i];
        stream.kind = resolved.kind;
        stream.source = resolved.selected_source;
        stream.signal = resolved.signal;
        stream.name = resolved.name;
        stream.required = resolved.required;
        stream.complete = resolved.complete;
        stream.has_edf_segment = resolved.has_edf_segment;
        stream.has_spool_segment = resolved.has_spool_segment;
    }
    range_plot_.state().range_count =
        std::min(plan.range_count,
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_NIGHT_SESSION_MAX; ++i) {
        if (i < range_plot_.state().range_count) {
            range_plot_.state().ranges[i].start_ms = plan.ranges[i].start_ms;
            range_plot_.state().ranges[i].end_ms = plan.ranges[i].end_ms;
        } else {
            range_plot_.state().ranges[i] =
                ReportSessionRange{};
        }
    }

    EdfReportDataProvider edf_provider(range_plot_.state().edf_sessions,
                                       range_plot_.state().edf_session_count);
    for (size_t i = 0; i < plan.segment_count; ++i) {
        const ReportResolvedSegment &segment = plan.segments[i];
        if (segment.stream_index >= range_plot_.state().stream_count) {
            fail("range_bad_segment");
            return false;
        }
        if (!segment.complete ||
            segment.provider == ReportResolvedProvider::None) {
            continue;
        }
        const ReportSourceDef *source_def = report_source_def(segment.source);
        if (!source_def || !source_def->spool_type ||
            !source_def->spool_type[0]) {
            fail("range_bad_source");
            return false;
        }
        const ReportDataProvider *provider = nullptr;
        if (segment.provider == ReportResolvedProvider::Edf) {
            provider = &edf_provider;
        } else if (segment.provider == ReportResolvedProvider::Spool) {
            provider = &spool_report_provider();
        }
        if (!provider) {
            fail("range_bad_provider");
            return false;
        }
        RangeChunkContext context;
        context.builder = this;
        context.stream_index = segment.stream_index;
        context.name = segment.name;
        if (!provider->for_each_chunk(segment.kind,
                                      *source_def,
                                      segment.signal,
                                      segment.name,
                                      static_cast<int64_t>(
                                          night.summary.start_ms),
                                      segment.start_ms,
                                      segment.end_ms,
                                      collect_chunk,
                                      &context)) {
            fail("range_chunk_list_failed");
            return false;
        }
    }
    return true;
}

}  // namespace aircannect
