#include "report_result_provider_bridge.h"

#include "edf_report_data_plan.h"
#include "edf_report_provider.h"
#include "edf_report_provider_token.h"
#include "report_manager_helpers.h"
#include "string_util.h"

#include <string.h>

namespace aircannect {
namespace {

using ReportResultChunk = report_manager_internal::ReportResultChunk;

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

const EdfReportProvider &edf_report_provider() {
    static EdfReportProvider provider;
    return provider;
}

}  // namespace

bool report_read_result_chunk_payload(
    const ReportResultChunk &chunk,
    int64_t night_start_ms,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload) {
    ReportProviderChunk provider_chunk;
    report_provider_chunk_from_result(chunk, provider_chunk);

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().read_chunk(
                provider_chunk,
                night_start_ms,
                meta,
                payload);
        case ReportProviderId::Edf:
            return edf_report_provider().read_chunk(provider_chunk,
                                                   sessions,
                                                   session_count,
                                                   meta,
                                                   payload);
        default:
            return false;
    }
}

void report_provider_chunk_from_result(
    const ReportResultChunk &chunk,
    ReportProviderChunk &out) {
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

bool report_result_chunk_has_stream(const ReportResultChunk &chunk,
                                    size_t stream_index) {
    uint32_t bit = 0;
    if (chunk.stream_mask != 0 &&
        report_manager_internal::report_stream_bit(stream_index, bit)) {
        return (chunk.stream_mask & bit) != 0;
    }
    return stream_index == chunk.stream_index;
}

bool report_result_chunk_same_physical_edf(
    const ReportResultChunk &existing,
    const ReportProviderChunk &candidate) {
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

bool report_result_chunk_matches_stream(
    const ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream &stream) {
    if (!report_result_chunk_has_stream(chunk, stream_index)) return false;

    if (chunk.stream_mask != 0) {
        return chunk.kind == stream.kind;
    }

    return chunk.kind == stream.kind &&
           chunk.signal == stream.signal &&
           chunk.name && stream.name &&
           strcmp(chunk.name, stream.name) == 0;
}

bool report_provider_chunk_from_result_stream(
    const ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream *streams,
    size_t stream_count,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderChunk &out) {
    if (!streams || stream_index >= stream_count ||
        !report_result_chunk_has_stream(chunk, stream_index)) {
        return false;
    }

    const ReportResultStream &stream = streams[stream_index];
    report_provider_chunk_from_result(chunk, out);
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

bool report_for_each_result_series_sample(
    const ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream *streams,
    size_t stream_count,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    int64_t night_start_ms,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) {
    stats = {};
    if (!callback || chunk.kind != ReportStoreChunkKind::Series) {
        return false;
    }

    ReportProviderChunk provider_chunk;
    if (!report_provider_chunk_from_result_stream(chunk,
                                                  stream_index,
                                                  streams,
                                                  stream_count,
                                                  sessions,
                                                  session_count,
                                                  provider_chunk)) {
        return false;
    }

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().for_each_series_sample(
                provider_chunk,
                night_start_ms,
                stats,
                callback,
                context);
        case ReportProviderId::Edf:
            return edf_report_provider().for_each_series_sample(
                provider_chunk,
                sessions,
                session_count,
                stats,
                callback,
                context);
        default:
            return false;
    }
}

}  // namespace aircannect
