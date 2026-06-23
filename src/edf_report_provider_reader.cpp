#include "edf_report_provider.h"

#include "edf_report_data_plan.h"
#include "edf_report_data_reader.h"
#include "edf_report_provider_token.h"
#include "string_util.h"

namespace aircannect {

bool EdfReportProvider::read_chunk(
    const ReportProviderChunk &chunk,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload) const {
    meta = {};
    payload.clear();
    EdfReportProviderToken token;
    if (!edf_report_provider_unpack_token(chunk.ref, token) ||
        !sessions ||
        token.session_index >= session_count ||
        !chunk.name || !chunk.name[0]) {
        return false;
    }

    EdfReportDataPlanEntry entry;
    entry.kind = token.data_kind;
    entry.signal = chunk.signal;
    entry.source = chunk.source;
    entry.name = chunk.name;
    entry.file_kind = token.file_kind;
    entry.file_slot = token.file_slot;
    copy_cstr(entry.signal_label,
              sizeof(entry.signal_label),
              token.signal_label);
    entry.first_record = token.first_record;
    entry.record_count = token.record_count;
    entry.start_ms = chunk.start_ms;
    entry.end_ms = chunk.end_ms;
    entry.record_count_estimate = chunk.record_count;
    entry.payload_len_estimate = chunk.payload_len;
    entry.primary = token.primary;

    EdfReportDataReadStats stats;
    const EdfReportDataReadStatus status = edf_report_read_entry_payload(
        sessions[token.session_index],
        entry,
        meta,
        payload,
        stats);
    return status == EdfReportDataReadStatus::Ok;
}

}  // namespace aircannect
