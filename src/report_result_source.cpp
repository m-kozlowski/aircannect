#include "report_manager.h"

#include <limits.h>
#include <string.h>

#include "edf_report_data_plan.h"
#include "edf_report_provider.h"
#include "edf_report_provider_token.h"
#include "report_data_provider.h"
#include "report_sources.h"
#include "string_util.h"

namespace aircannect {
namespace {

struct ResultChunkContext {
    ReportManager *manager = nullptr;
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Flow;
    const char *name = nullptr;
    bool required = false;
    size_t stream_index = SIZE_MAX;
    uint32_t entries = 0;
};

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

const EdfReportProvider &edf_report_provider() {
    static EdfReportProvider provider;
    return provider;
}

bool report_stream_bit(size_t stream_index, uint32_t &bit) {
    if (stream_index >= 32) return false;
    bit = 1u << static_cast<uint32_t>(stream_index);
    return true;
}

}  // namespace

bool ReportManager::collect_result_chunk(void *context,
                                         const ReportProviderChunk &info) {
    ResultChunkContext *ctx = static_cast<ResultChunkContext *>(context);
    if (!ctx || !ctx->manager || !info.name || !info.name[0]) return false;

    ReportManager *manager = ctx->manager;
    if (ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) != 0) {
        return true;
    }

    for (uint32_t i = 0; i < manager->result_status_.chunk_count; ++i) {
        const ReportResultChunk &existing = manager->result_chunks_[i];
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
        if (stream_index >= manager->result_stream_count_) {
            manager->fail_result_prepare("bad_result_stream");
            return false;
        }

        const ReportResultStream &stream =
            manager->result_streams_[stream_index];
        if (stream.kind != info.kind ||
            stream.signal != info.signal ||
            !stream.name ||
            strcmp(stream.name, info.name) != 0) {
            manager->fail_result_prepare("result_stream_mismatch");
            return false;
        }
    }

    if (stream_index == SIZE_MAX || !fixed_stream) {
        if (!manager->add_result_stream(info.kind,
                                        info.source,
                                        info.signal,
                                        info.name,
                                        ctx->required,
                                        true,
                                        stream_index)) {
            return false;
        }
    }

    if (!manager->add_provider_result_chunk(info,
                                            ctx->required,
                                            stream_index)) {
        return false;
    }
    if (fixed_stream) ctx->stream_index = stream_index;
    ctx->entries++;
    return true;
}

bool ReportManager::add_provider_result_chunk(
    const ReportProviderChunk &provider_chunk,
    bool required,
    size_t stream_index) {
    (void)required;
    if (stream_index >= result_stream_count_ || stream_index > UINT8_MAX) {
        fail_result_prepare("bad_result_stream");
        return false;
    }

    uint32_t stream_bit = 0;
    if (!report_stream_bit(stream_index, stream_bit)) {
        fail_result_prepare("bad_result_stream");
        return false;
    }

    if (!result_chunks_) {
        fail_result_prepare("result_chunks_missing");
        return false;
    }

    auto account_stream = [&]() {
        ReportResultStream &stream = result_streams_[stream_index];
        stream.has_edf_segment =
            stream.has_edf_segment ||
            provider_chunk.ref.provider == ReportProviderId::Edf;
        stream.has_spool_segment =
            stream.has_spool_segment ||
            provider_chunk.ref.provider == ReportProviderId::Spool;
        stream.chunk_count++;
        stream.record_count += provider_chunk.record_count;
        stream.payload_bytes += provider_chunk.payload_len;
        result_status_.record_count += provider_chunk.record_count;
        result_status_.payload_bytes += provider_chunk.payload_len;
    };

    for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
        ReportResultChunk &existing = result_chunks_[i];
        const bool same_physical =
            result_chunk_same_physical_edf(existing, provider_chunk);
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

    if (result_status_.chunk_count >= result_chunk_capacity_) {
        fail_result_prepare("result_chunks_full");
        return false;
    }

    ReportResultChunk &chunk =
        result_chunks_[result_status_.chunk_count++];
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

bool ReportManager::add_provider_chunks_to_result_stream(
    const ReportDataProvider &provider,
    ReportStoreChunkKind kind,
    ReportSourceId source,
    ReportSignalId signal,
    const char *name,
    int64_t night_start_ms,
    int64_t start_ms,
    int64_t end_ms,
    bool required,
    bool complete,
    size_t stream_index) {
    if (!ensure_result_chunks()) return false;

    const ReportSourceDef *source_def = report_source_def(source);
    if (!source_def || !source_def->spool_type || !source_def->spool_type[0]) {
        fail_result_prepare("bad_result_source");
        return false;
    }

    const bool sparse_events = kind == ReportStoreChunkKind::Events;
    if (stream_index >= result_stream_count_ || !name || !name[0]) {
        fail_result_prepare("bad_result_stream");
        return false;
    }

    ReportResultStream &stream = result_streams_[stream_index];
    if (stream.kind != kind || stream.signal != signal || !stream.name ||
        strcmp(stream.name, name) != 0) {
        fail_result_prepare("result_stream_mismatch");
        return false;
    }

    if (required) stream.required = true;
    if (!complete && stream.complete) {
        stream.complete = false;
        if (stream.required) result_status_.missing_streams++;
    }
    if (!complete) return true;

    const uint32_t chunks_before = stream.chunk_count;
    ResultChunkContext context;
    context.manager = this;
    context.kind = kind;
    context.source = source;
    context.signal = signal;
    context.name = name;
    context.required = required;
    context.stream_index = stream_index;

    if (!provider.for_each_chunk(kind,
                                 *source_def,
                                 signal,
                                 name,
                                 night_start_ms,
                                 start_ms,
                                 end_ms,
                                 collect_result_chunk,
                                 &context)) {
        if (result_status_.state != ReportResultState::Error) {
            fail_result_prepare("result_chunk_list_failed");
        }
        return false;
    }

    if (!sparse_events &&
        required &&
        result_streams_[stream_index].chunk_count == chunks_before &&
        result_streams_[stream_index].complete) {
        result_streams_[stream_index].complete = false;
        result_status_.missing_streams++;
    }
    return true;
}

bool ReportManager::read_result_chunk_payload(
    const ReportResultChunk &chunk,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload) {
    ReportProviderChunk provider_chunk;
    provider_chunk_from_result(chunk, provider_chunk);

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().read_chunk(
                provider_chunk,
                static_cast<int64_t>(result_night_.start_ms),
                meta,
                payload);
        case ReportProviderId::Edf:
            return edf_report_provider().read_chunk(provider_chunk,
                                                   result_edf_sessions_,
                                                   result_edf_session_count_,
                                                   meta,
                                                   payload);
        default:
            return false;
    }
}

void ReportManager::provider_chunk_from_result(
    const ReportResultChunk &chunk,
    ReportProviderChunk &out) const {
    out = {};
    out.ref = chunk.provider_ref;
    out.kind = chunk.kind;
    out.source = chunk.source;
    out.signal = chunk.signal;
    out.name = chunk.name;
    out.start_ms = chunk.start_ms;
    out.end_ms = chunk.end_ms;
    out.payload_schema = chunk.payload_schema;
    out.record_count = chunk.record_count;
    out.payload_len = chunk.payload_len;
}

bool ReportManager::result_chunk_has_stream(const ReportResultChunk &chunk,
                                            size_t stream_index) const {
    uint32_t bit = 0;
    if (chunk.stream_mask != 0 && report_stream_bit(stream_index, bit)) {
        return (chunk.stream_mask & bit) != 0;
    }
    return stream_index == chunk.stream_index;
}

bool ReportManager::result_chunk_same_physical_edf(
    const ReportResultChunk &existing,
    const ReportProviderChunk &candidate) const {
    if (existing.kind != candidate.kind ||
        existing.provider_ref.provider != ReportProviderId::Edf ||
        candidate.ref.provider != ReportProviderId::Edf ||
        existing.payload_schema != candidate.payload_schema ||
        existing.start_ms != candidate.start_ms ||
        existing.end_ms != candidate.end_ms) {
        return false;
    }

    if (existing.kind == ReportStoreChunkKind::Series &&
        edf_report_signal_uses_edge_zero_padding(existing.signal) !=
            edf_report_signal_uses_edge_zero_padding(candidate.signal)) {
        return false;
    }

    EdfReportProviderToken a;
    EdfReportProviderToken b;
    if (!edf_report_provider_unpack_token(existing.provider_ref, a) ||
        !edf_report_provider_unpack_token(candidate.ref, b)) {
        return false;
    }
    return a.session_index == b.session_index &&
           a.file_slot == b.file_slot &&
           a.file_kind == b.file_kind &&
           a.data_kind == b.data_kind &&
           a.first_record == b.first_record &&
           a.record_count == b.record_count;
}

bool ReportManager::provider_chunk_from_result_stream(
    const ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream *streams,
    size_t stream_count,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderChunk &out) const {
    if (!streams || stream_index >= stream_count ||
        !result_chunk_has_stream(chunk, stream_index)) {
        return false;
    }

    const ReportResultStream &stream = streams[stream_index];
    provider_chunk_from_result(chunk, out);
    out.source = stream.source;
    out.signal = stream.signal;
    out.name = stream.name;

    if (chunk.provider_ref.provider != ReportProviderId::Edf ||
        chunk.kind != ReportStoreChunkKind::Series) {
        return true;
    }

    EdfReportProviderToken token;
    if (!edf_report_provider_unpack_token(chunk.provider_ref, token) ||
        token.session_index >= session_count || !sessions) {
        return false;
    }

    EdfReportDataPlanEntry entry;
    if (!edf_report_find_signal_entry_for_chunk(
            sessions[token.session_index],
            stream.signal,
            token.file_kind,
            token.file_slot,
            token.first_record,
            token.record_count,
            chunk.start_ms,
            chunk.end_ms,
            entry)) {
        return false;
    }

    token.primary = entry.primary;
    token.trim_leading_padding = entry.trim_leading_padding;
    token.trim_trailing_padding = entry.trim_trailing_padding;
    copy_cstr(token.signal_label,
              sizeof(token.signal_label),
              entry.signal_label);
    edf_report_provider_pack_token(out.ref, token);
    out.source = entry.source;
    out.signal = entry.signal;
    out.name = entry.name;
    out.start_ms = entry.start_ms;
    out.end_ms = entry.end_ms;
    out.record_count = entry.record_count_estimate;
    out.payload_len = entry.payload_len_estimate;
    return true;
}

bool ReportManager::for_each_result_series_sample(
    const ReportResultChunk &chunk,
    size_t stream_index,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) {
    stats = {};
    if (!callback || chunk.kind != ReportStoreChunkKind::Series) {
        return false;
    }

    ReportProviderChunk provider_chunk;
    if (!provider_chunk_from_result_stream(chunk,
                                           stream_index,
                                           result_streams_,
                                           result_stream_count_,
                                           result_edf_sessions_,
                                           result_edf_session_count_,
                                           provider_chunk)) {
        return false;
    }

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().for_each_series_sample(
                provider_chunk,
                static_cast<int64_t>(result_night_.start_ms),
                stats,
                callback,
                context);
        case ReportProviderId::Edf:
            return edf_report_provider().for_each_series_sample(
                provider_chunk,
                result_edf_sessions_,
                result_edf_session_count_,
                stats,
                callback,
                context);
        default:
            return false;
    }
}

}  // namespace aircannect
