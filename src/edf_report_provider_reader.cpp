#include "edf_report_provider.h"

#include "edf_report_data_plan.h"
#include "edf_report_data_reader.h"
#include "edf_report_provider_token.h"
#include "debug_log.h"
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
    if (status != EdfReportDataReadStatus::Ok) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "EDF provider read failed status=%s kind=%u source=%u "
                  "signal=%u name=%s session=%u file_slot=%u primary=%u "
                  "first_record=%lu records=%lu estimate_records=%lu "
                  "estimate_bytes=%lu read_records=%lu samples=%lu "
                  "emitted=%lu missing=%lu\n",
                  edf_report_data_read_status_name(status),
                  static_cast<unsigned>(entry.kind),
                  static_cast<unsigned>(entry.source),
                  static_cast<unsigned>(entry.signal),
                  chunk.name,
                  static_cast<unsigned>(token.session_index),
                  static_cast<unsigned>(token.file_slot),
                  token.primary ? 1u : 0u,
                  static_cast<unsigned long>(entry.first_record),
                  static_cast<unsigned long>(entry.record_count),
                  static_cast<unsigned long>(entry.record_count_estimate),
                  static_cast<unsigned long>(entry.payload_len_estimate),
                  static_cast<unsigned long>(stats.records_read),
                  static_cast<unsigned long>(stats.samples_seen),
                  static_cast<unsigned long>(stats.samples_emitted),
                  static_cast<unsigned long>(stats.samples_missing));
    }
    return status == EdfReportDataReadStatus::Ok;
}

}  // namespace aircannect
