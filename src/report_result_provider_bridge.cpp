#include "report_manager.h"

#include "edf_report_data_plan.h"
#include "edf_report_provider.h"
#include "edf_report_provider_token.h"
#include "report_data_provider.h"
#include "string_util.h"

namespace aircannect {
namespace {

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
