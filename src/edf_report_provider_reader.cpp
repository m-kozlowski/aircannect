#include "edf_report_provider.h"

#include "edf_report_data_plan.h"
#include "edf_report_data_reader.h"
#include "edf_report_provider_token.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

bool entry_from_chunk(const ReportProviderChunk &chunk,
                      const EdfReportSessionDescriptor *sessions,
                      size_t session_count,
                      EdfReportProviderToken &token,
                      EdfReportDataPlanEntry &entry) {
    entry = {};
    if (!edf_report_provider_unpack_token(chunk.ref, token) ||
        !sessions ||
        token.session_index >= session_count ||
        !chunk.name || !chunk.name[0]) {
        return false;
    }

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
    entry.trim_leading_padding = token.trim_leading_padding;
    entry.trim_trailing_padding = token.trim_trailing_padding;
    return true;
}

void log_edf_provider_read_failure(const char *op,
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
              op,
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

struct CompatibleBatchCallbackContext {
    EdfReportSeriesBatchSampleCallback callback = nullptr;
    void *context = nullptr;
    const size_t *candidate_indices = nullptr;
    size_t candidate_count = 0;
};

bool emit_compatible_batch_sample(void *context,
                                  size_t item_index,
                                  const ReportSeriesSample &sample) {
    CompatibleBatchCallbackContext *batch =
        static_cast<CompatibleBatchCallbackContext *>(context);
    if (!batch || !batch->callback ||
        !batch->candidate_indices ||
        item_index >= batch->candidate_count) {
        return false;
    }
    return batch->callback(batch->context,
                           batch->candidate_indices[item_index],
                           sample);
}

struct CompatiblePlotBatchCallbackContext {
    EdfReportSeriesBatchPlotCallback callback = nullptr;
    void *context = nullptr;
    const size_t *candidate_indices = nullptr;
    size_t candidate_count = 0;
};

bool emit_compatible_plot_batch_point(
    void *context,
    size_t item_index,
    const EdfReportSeriesPlotPoint &point) {
    CompatiblePlotBatchCallbackContext *batch =
        static_cast<CompatiblePlotBatchCallbackContext *>(context);
    if (!batch || !batch->callback ||
        !batch->candidate_indices ||
        item_index >= batch->candidate_count) {
        return false;
    }
    return batch->callback(batch->context,
                           batch->candidate_indices[item_index],
                           point);
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
    if (!entry_from_chunk(chunk, sessions, session_count, token, entry)) {
        return false;
    }

    EdfReportDataReadStats stats;
    const EdfReportDataReadStatus status = edf_report_read_entry_payload(
        sessions[token.session_index],
        entry,
        meta,
        payload,
        stats);
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
    if (!entry_from_chunk(chunk, sessions, session_count, token, entry)) {
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

bool EdfReportProvider::for_each_compatible_series_sample_batch(
    const ReportProviderChunk *chunks,
    size_t chunk_count,
    bool *selected,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderSeriesReadStats *read_stats,
    EdfReportSeriesBatchSampleCallback callback,
    void *context) const {
    if (selected) {
        for (size_t i = 0; i < chunk_count; ++i) selected[i] = false;
    }
    if (read_stats) {
        for (size_t i = 0; i < chunk_count; ++i) read_stats[i] = {};
    }
    if (!chunks || chunk_count == 0 || !sessions || session_count == 0 ||
        !selected || !callback) {
        return false;
    }

    EdfReportProviderToken seed_token;
    EdfReportDataPlanEntry seed_entry;
    if (!entry_from_chunk(chunks[0],
                          sessions,
                          session_count,
                          seed_token,
                          seed_entry) ||
        seed_entry.kind != EdfReportDataKind::Series) {
        return false;
    }
    if (chunk_count > SIZE_MAX / sizeof(size_t) ||
        chunk_count > SIZE_MAX / sizeof(EdfReportDataPlanEntry) ||
        chunk_count > SIZE_MAX / sizeof(EdfReportProviderToken) ||
        chunk_count > SIZE_MAX / sizeof(ReportStoreChunkMeta) ||
        chunk_count > SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }
    size_t *candidate_indices = static_cast<size_t *>(Memory::calloc_large(
        chunk_count, sizeof(size_t), false));
    EdfReportDataPlanEntry *entries =
        static_cast<EdfReportDataPlanEntry *>(Memory::calloc_large(
            chunk_count, sizeof(EdfReportDataPlanEntry), false));
    EdfReportProviderToken *tokens =
        static_cast<EdfReportProviderToken *>(Memory::calloc_large(
            chunk_count, sizeof(EdfReportProviderToken), false));
    ReportStoreChunkMeta *metas =
        static_cast<ReportStoreChunkMeta *>(Memory::calloc_large(
            chunk_count, sizeof(ReportStoreChunkMeta), false));
    uint32_t *intervals = static_cast<uint32_t *>(Memory::calloc_large(
        chunk_count, sizeof(uint32_t), false));
    if (!candidate_indices || !entries || !tokens || !metas || !intervals) {
        if (candidate_indices) Memory::free(candidate_indices);
        if (entries) Memory::free(entries);
        if (tokens) Memory::free(tokens);
        if (metas) Memory::free(metas);
        if (intervals) Memory::free(intervals);
        return false;
    }

    size_t selected_count = 0;
    for (size_t i = 0; i < chunk_count; ++i) {
        EdfReportProviderToken token;
        EdfReportDataPlanEntry entry;
        if (!entry_from_chunk(chunks[i],
                              sessions,
                              session_count,
                              token,
                              entry) ||
            entry.kind != EdfReportDataKind::Series ||
            token.session_index != seed_token.session_index ||
            token.file_slot != seed_token.file_slot ||
            token.file_kind != seed_token.file_kind ||
            token.data_kind != seed_token.data_kind) {
            continue;
        }
        selected[i] = true;
        candidate_indices[selected_count] = i;
        tokens[selected_count] = token;
        entries[selected_count] = entry;
        ++selected_count;
    }

    EdfReportDataReadStats stats;
    EdfReportDataReadStatus status = EdfReportDataReadStatus::InvalidArgument;
    if (selected_count > 0) {
        const uint32_t started_ms = millis();
        CompatibleBatchCallbackContext batch_context;
        batch_context.callback = callback;
        batch_context.context = context;
        batch_context.candidate_indices = candidate_indices;
        batch_context.candidate_count = selected_count;
        status = edf_report_for_each_series_batch_sample(
            sessions[seed_token.session_index],
            entries,
            selected_count,
            metas,
            stats,
            intervals,
            emit_compatible_batch_sample,
            &batch_context);
        if (read_stats) {
            for (size_t i = 0; i < selected_count; ++i) {
                const size_t candidate = candidate_indices[i];
                read_stats[candidate].record_count = metas[i].record_count;
                read_stats[candidate].interval_ms = intervals[i];
                read_stats[candidate].payload_bytes =
                    chunks[candidate].payload_len;
            }
        }
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "EDF provider batch session=%u file_slot=%u "
                  "entries=%lu records=%lu samples=%lu emitted=%lu "
                  "missing=%lu elapsed_ms=%lu\n",
                  static_cast<unsigned>(seed_token.session_index),
                  static_cast<unsigned>(seed_token.file_slot),
                  static_cast<unsigned long>(selected_count),
                  static_cast<unsigned long>(stats.records_read),
                  static_cast<unsigned long>(stats.samples_seen),
                  static_cast<unsigned long>(stats.samples_emitted),
                  static_cast<unsigned long>(stats.samples_missing),
                  static_cast<unsigned long>(millis() - started_ms));
    }
    if (status != EdfReportDataReadStatus::Ok) {
        log_edf_provider_read_failure("sample_batch",
                                      chunks[0],
                                      seed_token,
                                      seed_entry,
                                      status,
                                      stats);
    }

    Memory::free(intervals);
    Memory::free(metas);
    Memory::free(tokens);
    Memory::free(entries);
    Memory::free(candidate_indices);
    return status == EdfReportDataReadStatus::Ok;
}

bool EdfReportProvider::for_each_compatible_series_plot_batch(
    const ReportProviderChunk *chunks,
    size_t chunk_count,
    const EdfReportSeriesPlotConfig *configs,
    bool *selected,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderSeriesReadStats *read_stats,
    EdfReportSeriesBatchPlotCallback callback,
    void *context) const {
    if (selected) {
        for (size_t i = 0; i < chunk_count; ++i) selected[i] = false;
    }
    if (read_stats) {
        for (size_t i = 0; i < chunk_count; ++i) read_stats[i] = {};
    }
    if (!chunks || chunk_count == 0 || !configs || !sessions ||
        session_count == 0 || !selected || !callback) {
        return false;
    }

    EdfReportProviderToken seed_token;
    EdfReportDataPlanEntry seed_entry;
    if (!entry_from_chunk(chunks[0],
                          sessions,
                          session_count,
                          seed_token,
                          seed_entry) ||
        seed_entry.kind != EdfReportDataKind::Series ||
        edf_report_signal_uses_edge_zero_padding(seed_entry.signal)) {
        return false;
    }
    if (chunk_count > SIZE_MAX / sizeof(size_t) ||
        chunk_count > SIZE_MAX / sizeof(EdfReportDataPlanEntry) ||
        chunk_count > SIZE_MAX / sizeof(EdfReportProviderToken) ||
        chunk_count > SIZE_MAX / sizeof(EdfReportSeriesPlotConfig) ||
        chunk_count > SIZE_MAX / sizeof(ReportStoreChunkMeta) ||
        chunk_count > SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }
    size_t *candidate_indices = static_cast<size_t *>(Memory::calloc_large(
        chunk_count, sizeof(size_t), false));
    EdfReportDataPlanEntry *entries =
        static_cast<EdfReportDataPlanEntry *>(Memory::calloc_large(
            chunk_count, sizeof(EdfReportDataPlanEntry), false));
    EdfReportProviderToken *tokens =
        static_cast<EdfReportProviderToken *>(Memory::calloc_large(
            chunk_count, sizeof(EdfReportProviderToken), false));
    EdfReportSeriesPlotConfig *plot_configs =
        static_cast<EdfReportSeriesPlotConfig *>(Memory::calloc_large(
            chunk_count, sizeof(EdfReportSeriesPlotConfig), false));
    ReportStoreChunkMeta *metas =
        static_cast<ReportStoreChunkMeta *>(Memory::calloc_large(
            chunk_count, sizeof(ReportStoreChunkMeta), false));
    uint32_t *intervals = static_cast<uint32_t *>(Memory::calloc_large(
        chunk_count, sizeof(uint32_t), false));
    if (!candidate_indices || !entries || !tokens || !plot_configs ||
        !metas || !intervals) {
        if (candidate_indices) Memory::free(candidate_indices);
        if (entries) Memory::free(entries);
        if (tokens) Memory::free(tokens);
        if (plot_configs) Memory::free(plot_configs);
        if (metas) Memory::free(metas);
        if (intervals) Memory::free(intervals);
        return false;
    }

    size_t selected_count = 0;
    for (size_t i = 0; i < chunk_count; ++i) {
        EdfReportProviderToken token;
        EdfReportDataPlanEntry entry;
        if (!entry_from_chunk(chunks[i],
                              sessions,
                              session_count,
                              token,
                              entry) ||
            entry.kind != EdfReportDataKind::Series ||
            token.session_index != seed_token.session_index ||
            token.file_slot != seed_token.file_slot ||
            token.file_kind != seed_token.file_kind ||
            token.data_kind != seed_token.data_kind ||
            edf_report_signal_uses_edge_zero_padding(entry.signal) ||
            !configs[i].ranges || configs[i].range_count == 0) {
            continue;
        }
        selected[i] = true;
        candidate_indices[selected_count] = i;
        tokens[selected_count] = token;
        entries[selected_count] = entry;
        plot_configs[selected_count] = configs[i];
        ++selected_count;
    }

    EdfReportDataReadStats stats;
    EdfReportDataReadStatus status = EdfReportDataReadStatus::InvalidArgument;
    if (selected_count > 0) {
        const uint32_t started_ms = millis();
        CompatiblePlotBatchCallbackContext batch_context;
        batch_context.callback = callback;
        batch_context.context = context;
        batch_context.candidate_indices = candidate_indices;
        batch_context.candidate_count = selected_count;
        status = edf_report_for_each_series_batch_plot(
            sessions[seed_token.session_index],
            entries,
            selected_count,
            plot_configs,
            metas,
            stats,
            intervals,
            emit_compatible_plot_batch_point,
            &batch_context);
        if (read_stats) {
            for (size_t i = 0; i < selected_count; ++i) {
                const size_t candidate = candidate_indices[i];
                read_stats[candidate].record_count = metas[i].record_count;
                read_stats[candidate].interval_ms = intervals[i];
                read_stats[candidate].payload_bytes =
                    chunks[candidate].payload_len;
            }
        }
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "EDF provider plot_batch session=%u file_slot=%u "
                  "entries=%lu records=%lu samples=%lu emitted=%lu "
                  "missing=%lu elapsed_ms=%lu\n",
                  static_cast<unsigned>(seed_token.session_index),
                  static_cast<unsigned>(seed_token.file_slot),
                  static_cast<unsigned long>(selected_count),
                  static_cast<unsigned long>(stats.records_read),
                  static_cast<unsigned long>(stats.samples_seen),
                  static_cast<unsigned long>(stats.samples_emitted),
                  static_cast<unsigned long>(stats.samples_missing),
                  static_cast<unsigned long>(millis() - started_ms));
    }
    if (status != EdfReportDataReadStatus::Ok) {
        log_edf_provider_read_failure("plot_batch",
                                      chunks[0],
                                      seed_token,
                                      seed_entry,
                                      status,
                                      stats);
    }

    Memory::free(intervals);
    Memory::free(metas);
    Memory::free(plot_configs);
    Memory::free(tokens);
    Memory::free(entries);
    Memory::free(candidate_indices);
    return status == EdfReportDataReadStatus::Ok;
}

}  // namespace aircannect
