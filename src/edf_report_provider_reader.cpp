#include "edf_report_provider.h"

#include "debug_log.h"
#include "edf_report_data_plan.h"
#include "edf_report_data_reader.h"
#include "edf_report_provider_token.h"

namespace aircannect {
namespace {

void log_edf_provider_read_failure(const char *operation,
                                   const ReportProviderChunk &chunk,
                                   const EdfReportProviderToken &token,
                                   const EdfReportDataPlanEntry &entry,
                                   EdfReportDataReadStatus status,
                                   const EdfReportDataReadStats &stats) {
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "EDF provider %s failed status=%s kind=%u source=%u "
              "signal=%u name=%s session=%u file_slot=%u primary=%u "
              "first_record=%lu records=%lu estimate_records=%lu "
              "estimate_bytes=%lu read_records=%lu samples=%lu "
              "emitted=%lu missing=%lu\n",
              operation,
              edf_report_data_read_status_name(status),
              static_cast<unsigned>(entry.kind),
              static_cast<unsigned>(entry.source),
              static_cast<unsigned>(entry.signal),
              chunk.name ? chunk.name : "--",
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

}  // namespace

bool EdfReportProvider::read_chunk(
    const ReportProviderChunk &chunk,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload) const {
    meta = {};
    payload.clear();

    EdfReportProviderToken token;
    EdfReportDataPlanEntry entry;
    if (!sessions ||
        !edf_report_provider_entry_from_chunk(chunk,
                                              session_count,
                                              token,
                                              entry)) {
        return false;
    }

    EdfReportDataReadStats stats;
    const EdfReportDataReadStatus status = edf_report_read_entry_payload(
        sessions[token.session_index], entry, meta, payload, stats);
    if (status != EdfReportDataReadStatus::Ok) {
        log_edf_provider_read_failure("read",
                                      chunk,
                                      token,
                                      entry,
                                      status,
                                      stats);
    }
    return status == EdfReportDataReadStatus::Ok;
}

bool EdfReportProvider::for_each_series_sample(
    const ReportProviderChunk &chunk,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderSeriesReadStats &read_stats,
    ReportSeriesSampleCallback callback,
    void *context) const {
    read_stats = {};

    EdfReportProviderToken token;
    EdfReportDataPlanEntry entry;
    if (!sessions ||
        !edf_report_provider_entry_from_chunk(chunk,
                                              session_count,
                                              token,
                                              entry)) {
        return false;
    }

    ReportStoreChunkMeta meta;
    EdfReportDataReadStats stats;
    uint32_t interval_ms = 0;
    const EdfReportDataReadStatus status =
        edf_report_for_each_entry_series_sample(sessions[token.session_index],
                                                entry,
                                                meta,
                                                stats,
                                                &interval_ms,
                                                callback,
                                                context);
    read_stats.record_count = meta.record_count;
    read_stats.interval_ms = interval_ms;
    read_stats.payload_bytes = chunk.payload_len;
    if (status != EdfReportDataReadStatus::Ok) {
        log_edf_provider_read_failure("sample_iter",
                                      chunk,
                                      token,
                                      entry,
                                      status,
                                      stats);
    }
    return status == EdfReportDataReadStatus::Ok;
}

}  // namespace aircannect
