#include "report_result_materialization_sink.h"

#include <limits.h>
#include <string.h>

#include "edf_report_provider.h"
#include "report_manager_helpers.h"
#include "report_result_provider_bridge.h"
#include "report_sources.h"
#include "report_summary_json.h"

namespace aircannect {
namespace {

struct ResultChunkContext {
    ReportResultMaterializationSink *sink = nullptr;
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Flow;
    const char *name = nullptr;
    bool required = false;
    size_t stream_index = SIZE_MAX;
    uint32_t entries = 0;
};

}  // namespace

ReportResultMaterializationSink::ReportResultMaterializationSink(
    ReportResultRuntime &runtime,
    const ReportDataProvider &spool_provider)
    : runtime_(runtime), spool_provider_(spool_provider) {}

void ReportResultMaterializationSink::set_error(const char *message) {
    if (!error_) error_ = message;
}

bool ReportResultMaterializationSink::ensure_chunks() {
    if (runtime_.ensure_chunks()) return true;

    set_error("result_manifest_alloc_failed");
    return false;
}

bool ReportResultMaterializationSink::begin_materialization(
    const ReportIndexedNight &night,
    const ReportResolvedPlan &plan) {
    (void)runtime_.ranges().set_from_resolved_plan(plan);

    uint32_t duration_min = report_indexed_night_display_duration_min(night);
    if (duration_min == 0) {
        for (size_t i = 0; i < runtime_.ranges().count(); ++i) {
            duration_min += report_ceil_duration_min(
                runtime_.ranges()[i].start_ms,
                runtime_.ranges()[i].end_ms);
        }
    }

    if (duration_min > 0) {
        runtime_.status().duration_min = duration_min;
    } else if (night.has_summary && night.summary.duration_min > 0) {
        runtime_.status().duration_min = night.summary.duration_min;
    }

    return true;
}

bool ReportResultMaterializationSink::add_materialized_stream(
    const ReportResolvedStream &stream,
    size_t &result_stream_index) {
    if (!add_stream(stream.kind,
                    stream.selected_source,
                    stream.signal,
                    stream.name,
                    stream.required,
                    stream.complete,
                    result_stream_index)) {
        return false;
    }

    if (result_stream_index < runtime_.streams().count()) {
        ReportResultStream &result_stream =
            runtime_.streams()[result_stream_index];
        result_stream.has_edf_segment =
            result_stream.has_edf_segment || stream.has_edf_segment;
        result_stream.has_spool_segment =
            result_stream.has_spool_segment || stream.has_spool_segment;
    }

    return true;
}

bool ReportResultMaterializationSink::add_materialized_segment(
    const ReportResolvedSegment &segment,
    size_t result_stream_index) {
    if (segment.provider == ReportResolvedProvider::None) return true;

    const int64_t night_start_ms =
        static_cast<int64_t>(runtime_.identity().summary().start_ms);

    if (segment.provider == ReportResolvedProvider::Edf) {
        EdfReportDataProvider provider(runtime_.scratch().edf_sessions(),
                                       runtime_.scratch().edf_session_count());
        return add_provider_chunks_to_stream(provider,
                                             segment,
                                             night_start_ms,
                                             result_stream_index);
    }

    return add_provider_chunks_to_stream(spool_provider_,
                                         segment,
                                         night_start_ms,
                                         result_stream_index);
}

void ReportResultMaterializationSink::finish_materialization(
    const ReportResolvedPlan &plan) {
    runtime_.status().events_available = plan.events_available;
    runtime_.status().missing_required = runtime_.status().missing_streams;
}

bool ReportResultMaterializationSink::add_stream(ReportStoreChunkKind kind,
                                                 ReportSourceId source,
                                                 ReportSignalId signal,
                                                 const char *name,
                                                 bool required,
                                                 bool complete,
                                                 size_t &stream_index) {
    if (runtime_.add_stream(kind,
                            source,
                            signal,
                            name,
                            required,
                            complete,
                            stream_index)) {
        return true;
    }

    set_error("result_streams_full");
    return false;
}

bool ReportResultMaterializationSink::collect_chunk(
    void *context,
    const ReportProviderChunk &info) {
    ResultChunkContext *ctx = static_cast<ResultChunkContext *>(context);
    if (!ctx || !ctx->sink || !info.name || !info.name[0]) return false;

    ReportResultMaterializationSink &sink = *ctx->sink;
    if (ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) != 0) {
        return true;
    }

    for (uint32_t i = 0; i < sink.runtime_.status().chunk_count; ++i) {
        const ReportResultChunk &existing =
            sink.runtime_.scratch().chunks()[i];
        if (existing.kind == info.kind &&
            existing.source == info.source &&
            existing.name && info.name &&
            strcmp(existing.name, info.name) == 0 &&
            existing.start_ms == info.start_ms &&
            existing.end_ms == info.end_ms &&
            report_provider_chunk_ref_equal(existing.provider_ref,
                                            info.ref)) {
            return true;
        }
    }

    const bool fixed_stream =
        ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) == 0;
    size_t stream_index = ctx->stream_index;
    if (fixed_stream && stream_index != SIZE_MAX) {
        if (stream_index >= sink.runtime_.streams().count()) {
            sink.set_error("bad_result_stream");
            return false;
        }

        const ReportResultStream &stream =
            sink.runtime_.streams()[stream_index];
        if (stream.kind != info.kind ||
            stream.signal != info.signal ||
            !stream.name ||
            strcmp(stream.name, info.name) != 0) {
            sink.set_error("result_stream_mismatch");
            return false;
        }
    }

    if (stream_index == SIZE_MAX || !fixed_stream) {
        if (!sink.add_stream(info.kind,
                             info.source,
                             info.signal,
                             info.name,
                             ctx->required,
                             true,
                             stream_index)) {
            return false;
        }
    }

    if (!sink.add_provider_chunk(info, ctx->required, stream_index)) {
        return false;
    }

    if (fixed_stream) ctx->stream_index = stream_index;
    ctx->entries++;
    return true;
}

bool ReportResultMaterializationSink::add_provider_chunk(
    const ReportProviderChunk &provider_chunk,
    bool required,
    size_t stream_index) {
    (void)required;
    if (stream_index >= runtime_.streams().count() || stream_index > UINT8_MAX) {
        set_error("bad_result_stream");
        return false;
    }

    uint32_t stream_bit = 0;
    if (!report_manager_internal::report_stream_bit(stream_index, stream_bit)) {
        set_error("bad_result_stream");
        return false;
    }

    if (!runtime_.scratch().chunks()) {
        set_error("result_chunks_missing");
        return false;
    }

    auto account_stream = [&]() {
        ReportResultStream &stream = runtime_.streams()[stream_index];
        stream.has_edf_segment =
            stream.has_edf_segment ||
            provider_chunk.ref.provider == ReportProviderId::Edf;
        stream.has_spool_segment =
            stream.has_spool_segment ||
            provider_chunk.ref.provider == ReportProviderId::Spool;
        stream.chunk_count++;
        stream.record_count += provider_chunk.record_count;
        stream.payload_bytes += provider_chunk.payload_len;
        runtime_.status().record_count += provider_chunk.record_count;
        runtime_.status().payload_bytes += provider_chunk.payload_len;
    };

    for (uint32_t i = 0; i < runtime_.status().chunk_count; ++i) {
        ReportResultChunk &existing = runtime_.scratch().chunks()[i];
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

    if (runtime_.status().chunk_count >= runtime_.scratch().chunk_capacity()) {
        set_error("result_chunks_full");
        return false;
    }

    ReportResultChunk &chunk =
        runtime_.scratch().chunks()[runtime_.status().chunk_count++];
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

bool ReportResultMaterializationSink::add_provider_chunks_to_stream(
    const ReportDataProvider &provider,
    const ReportResolvedSegment &segment,
    int64_t night_start_ms,
    size_t stream_index) {
    if (!ensure_chunks()) return false;

    const ReportSourceDef *source_def = report_source_def(segment.source);
    if (!source_def || !source_def->spool_type || !source_def->spool_type[0]) {
        set_error("bad_result_source");
        return false;
    }

    const bool sparse_events = segment.kind == ReportStoreChunkKind::Events;
    if (stream_index >= runtime_.streams().count() ||
        !segment.name || !segment.name[0]) {
        set_error("bad_result_stream");
        return false;
    }

    ReportResultStream &stream = runtime_.streams()[stream_index];
    if (stream.kind != segment.kind ||
        stream.signal != segment.signal ||
        !stream.name ||
        strcmp(stream.name, segment.name) != 0) {
        set_error("result_stream_mismatch");
        return false;
    }

    if (segment.required) stream.required = true;
    if (!segment.complete && stream.complete) {
        stream.complete = false;
        if (stream.required) runtime_.status().missing_streams++;
    }
    if (!segment.complete) return true;

    const uint32_t chunks_before = stream.chunk_count;

    ResultChunkContext context;
    context.sink = this;
    context.kind = segment.kind;
    context.source = segment.source;
    context.signal = segment.signal;
    context.name = segment.name;
    context.required = segment.required;
    context.stream_index = stream_index;

    if (!provider.for_each_chunk(segment.kind,
                                 *source_def,
                                 segment.signal,
                                 segment.name,
                                 night_start_ms,
                                 segment.start_ms,
                                 segment.end_ms,
                                 collect_chunk,
                                 &context)) {
        if (!error_) set_error("result_chunk_list_failed");
        return false;
    }

    if (!sparse_events &&
        segment.required &&
        runtime_.streams()[stream_index].chunk_count == chunks_before &&
        runtime_.streams()[stream_index].complete) {
        runtime_.streams()[stream_index].complete = false;
        runtime_.status().missing_streams++;
    }

    return true;
}

}  // namespace aircannect
