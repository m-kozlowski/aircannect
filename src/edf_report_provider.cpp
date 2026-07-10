#include "edf_report_provider.h"

#include <limits.h>
#include <string.h>

#include "edf_report_data_plan.h"
#include "edf_report_provider_token.h"
#include "report_records.h"
#include "string_util.h"

namespace aircannect {
namespace {

struct EdfProviderPlanContext {
    const EdfReportProvider *provider = nullptr;
    uint8_t session_index = 0;
    ReportProviderChunkCallback callback = nullptr;
    void *context = nullptr;
    uint32_t entries = 0;
};

bool emit_edf_provider_chunk(void *context,
                             const EdfReportDataPlanEntry &entry) {
    EdfProviderPlanContext *ctx =
        static_cast<EdfProviderPlanContext *>(context);
    if (!ctx || !ctx->provider || !ctx->callback ||
        !entry.name || !entry.name[0]) {
        return false;
    }

    EdfReportProviderToken token;
    token.session_index = ctx->session_index;
    token.file_slot = entry.file_slot;
    token.file_kind = entry.file_kind;
    token.data_kind = entry.kind;
    token.primary = entry.primary;
    token.trim_leading_padding = entry.trim_leading_padding;
    token.trim_trailing_padding = entry.trim_trailing_padding;
    token.first_record = entry.first_record;
    token.record_count = entry.record_count;
    copy_cstr(token.signal_label,
              sizeof(token.signal_label),
              entry.signal_label);

    ReportProviderChunk chunk;
    edf_report_provider_pack_token(chunk.ref, token);
    chunk.kind = entry.kind == EdfReportDataKind::Events
                     ? ReportStoreChunkKind::Events
                     : ReportStoreChunkKind::Series;
    chunk.source = entry.source;
    chunk.signal = entry.signal;
    chunk.name = entry.name;
    chunk.start_ms = entry.start_ms;
    chunk.end_ms = entry.end_ms;
    chunk.payload_schema =
        chunk.kind == ReportStoreChunkKind::Events
            ? REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1
            : REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    chunk.record_count =
        entry.record_count_estimate ? entry.record_count_estimate
                                    : entry.record_count;
    chunk.payload_len = entry.payload_len_estimate;

    if (!ctx->callback(ctx->context, chunk)) return false;
    ctx->entries++;
    return true;
}

bool valid_sessions(const EdfReportSessionDescriptor *sessions,
                    size_t session_count) {
    return sessions && session_count > 0;
}

bool session_has_event_file(const EdfReportSessionDescriptor &session,
                            EdfInventoryFileKind kind) {
    return edf_report_session_has_file(session, kind);
}

bool session_covers_range(const EdfReportSessionDescriptor &session,
                          int64_t start_ms,
                          int64_t end_ms) {
    if (end_ms <= start_ms || session.latest_header_end_ms <= 0) return false;

    const int64_t session_start_ms =
        session.earliest_header_start_ms - AC_EDF_REPORT_COVERAGE_TOLERANCE_MS;
    const int64_t session_end_ms =
        session.latest_header_end_ms + AC_EDF_REPORT_COVERAGE_TOLERANCE_MS;
    return session_start_ms <= start_ms && session_end_ms >= end_ms;
}

bool event_file_covers_range(const EdfReportSessionDescriptor *sessions,
                             size_t session_count,
                             EdfInventoryFileKind kind,
                             int64_t start_ms,
                             int64_t end_ms) {
    if (!valid_sessions(sessions, session_count)) return false;
    for (size_t i = 0; i < session_count; ++i) {
        if (session_has_event_file(sessions[i], kind) &&
            session_covers_range(sessions[i], start_ms, end_ms)) {
            return true;
        }
    }
    return false;
}

struct EdfProviderFilterContext {
    const char *name = nullptr;
    ReportProviderChunkCallback callback = nullptr;
    void *context = nullptr;
};

bool filter_edf_provider_chunk(void *context,
                               const ReportProviderChunk &chunk) {
    EdfProviderFilterContext *ctx =
        static_cast<EdfProviderFilterContext *>(context);
    if (!ctx || !ctx->callback) return false;
    if (ctx->name && ctx->name[0] &&
        (!chunk.name || strcmp(chunk.name, ctx->name) != 0)) {
        return true;
    }
    return ctx->callback(ctx->context, chunk);
}

struct EdfProviderExtentContext {
    bool found = false;
    int64_t min_start_ms = 0;
    int64_t max_end_ms = 0;
};

bool remember_edf_provider_extent(void *context,
                                  const ReportProviderChunk &chunk) {
    EdfProviderExtentContext *ctx =
        static_cast<EdfProviderExtentContext *>(context);
    if (!ctx) return false;
    if (!ctx->found || chunk.start_ms < ctx->min_start_ms) {
        ctx->min_start_ms = chunk.start_ms;
    }
    if (!ctx->found || chunk.end_ms > ctx->max_end_ms) {
        ctx->max_end_ms = chunk.end_ms;
    }
    ctx->found = true;
    return true;
}

bool signal_for_store_name(const char *name, ReportSignalId &out) {
    if (!name || !name[0]) return false;
    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t i = 0; i < signal_count; ++i) {
        if (signals[i].store_name && strcmp(signals[i].store_name, name) == 0) {
            out = signals[i].id;
            return true;
        }
    }
    return false;
}

}  // namespace

bool EdfReportProvider::for_each_event_chunk(
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    int64_t range_start_ms,
    int64_t range_end_ms,
    ReportProviderChunkCallback callback,
    void *context) const {
    if (!valid_sessions(sessions, session_count) || !callback ||
        range_end_ms <= range_start_ms) {
        return false;
    }

    for (size_t i = 0; i < session_count; ++i) {
        if (i > UINT8_MAX) return false;
        EdfProviderPlanContext ctx;
        ctx.provider = this;
        ctx.session_index = static_cast<uint8_t>(i);
        ctx.callback = callback;
        ctx.context = context;
        if (!edf_report_plan_events(sessions[i],
                                    range_start_ms,
                                    range_end_ms,
                                    emit_edf_provider_chunk,
                                    &ctx)) {
            return false;
        }
    }
    return true;
}

bool EdfReportProvider::for_each_signal_chunk(
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportSignalId signal,
    int64_t range_start_ms,
    int64_t range_end_ms,
    ReportProviderChunkCallback callback,
    void *context) const {
    if (!valid_sessions(sessions, session_count) || !callback ||
        range_end_ms <= range_start_ms) {
        return false;
    }

    for (size_t i = 0; i < session_count; ++i) {
        if (i > UINT8_MAX) return false;
        EdfProviderPlanContext ctx;
        ctx.provider = this;
        ctx.session_index = static_cast<uint8_t>(i);
        ctx.callback = callback;
        ctx.context = context;
        if (!edf_report_plan_signal(sessions[i],
                                    signal,
                                    range_start_ms,
                                    range_end_ms,
                                    emit_edf_provider_chunk,
                                    &ctx)) {
            return false;
        }
    }
    return true;
}

bool EdfReportDataProvider::coverage_complete(const ReportSourceDef &source,
                                              int64_t start_ms,
                                              int64_t end_ms) const {
    if (end_ms <= start_ms) return false;
    if (source.id == ReportSourceId::RespiratoryEvents) {
        return event_file_covers_range(sessions_,
                                       session_count_,
                                       EdfInventoryFileKind::Eve,
                                       start_ms,
                                       end_ms);
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    bool matched = false;
    for (size_t i = 0; i < signal_count; ++i) {
        if (signals[i].preferred_source != source.id &&
            signals[i].fallback_source != source.id) {
            continue;
        }
        matched = true;
        EdfReportRequiredRange range;
        range.start_ms = start_ms;
        range.end_ms = end_ms;
        if (!edf_report_plan_signal_covers_ranges(sessions_,
                                                  session_count_,
                                                  signals[i].id,
                                                  &range,
                                                  1)) {
            return false;
        }
    }
    return matched;
}

bool EdfReportDataProvider::coverage_first_missing(
    const ReportSourceDef &source,
    int64_t start_ms,
    int64_t end_ms,
    int64_t &missing_ms) const {
    missing_ms = start_ms;
    if (coverage_complete(source, start_ms, end_ms)) {
        missing_ms = end_ms;
        return false;
    }
    return true;
}

bool EdfReportDataProvider::signal_coverage_complete(
    const ReportSourceDef &source,
    ReportSignalId signal,
    int64_t start_ms,
    int64_t end_ms) const {
    (void)source;
    if (end_ms <= start_ms) return false;

    EdfReportRequiredRange range;
    range.start_ms = start_ms;
    range.end_ms = end_ms;
    return edf_report_plan_signal_covers_ranges(sessions_,
                                                session_count_,
                                                signal,
                                                &range,
                                                1);
}

bool EdfReportDataProvider::for_each_chunk(
    ReportStoreChunkKind kind,
    const ReportSourceDef &source,
    ReportSignalId signal,
    const char *name,
    int64_t night_start_ms,
    int64_t start_ms,
    int64_t end_ms,
    ReportProviderChunkCallback callback,
    void *context) const {
    (void)source;
    (void)night_start_ms;
    if (!callback || end_ms <= start_ms) return false;
    if (!valid_sessions(sessions_, session_count_)) return true;

    EdfProviderFilterContext filter;
    filter.name = name;
    filter.callback = callback;
    filter.context = context;

    const EdfReportProvider provider;
    if (kind == ReportStoreChunkKind::Events) {
        return provider.for_each_event_chunk(sessions_,
                                             session_count_,
                                             start_ms,
                                             end_ms,
                                             filter_edf_provider_chunk,
                                             &filter);
    }
    return provider.for_each_signal_chunk(sessions_,
                                          session_count_,
                                          signal,
                                          start_ms,
                                          end_ms,
                                          filter_edf_provider_chunk,
                                          &filter);
}

bool EdfReportDataProvider::read_chunk(const ReportProviderChunk &chunk,
                                       int64_t night_start_ms,
                                       ReportStoreChunkMeta &meta,
                                       ReportSpoolBuffer &payload) const {
    (void)night_start_ms;
    const EdfReportProvider provider;
    return provider.read_chunk(chunk, sessions_, session_count_, meta, payload);
}

bool EdfReportDataProvider::for_each_series_sample(
    const ReportProviderChunk &chunk,
    int64_t night_start_ms,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) const {
    (void)night_start_ms;
    const EdfReportProvider provider;
    return provider.for_each_series_sample(chunk,
                                           sessions_,
                                           session_count_,
                                           stats,
                                           callback,
                                           context);
}

#ifndef ARDUINO
bool EdfReportProvider::read_chunk(const ReportProviderChunk &chunk,
                                   const EdfReportSessionDescriptor *sessions,
                                   size_t session_count,
                                   ReportStoreChunkMeta &meta,
                                   ReportSpoolBuffer &payload) const {
    (void)chunk;
    (void)sessions;
    (void)session_count;
    meta = {};
    payload.clear();
    return false;
}

bool EdfReportProvider::for_each_series_sample(
    const ReportProviderChunk &chunk,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) const {
    (void)chunk;
    (void)sessions;
    (void)session_count;
    (void)callback;
    (void)context;
    stats = {};
    return false;
}

bool EdfReportProvider::for_each_compatible_series_sample_batch(
    const ReportProviderChunk *chunks,
    size_t chunk_count,
    bool *selected,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderSeriesReadStats *stats,
    EdfReportSeriesBatchSampleCallback callback,
    void *context) const {
    (void)chunks;
    (void)sessions;
    (void)session_count;
    (void)callback;
    (void)context;
    if (selected) {
        for (size_t i = 0; i < chunk_count; ++i) selected[i] = false;
    }
    if (stats) {
        for (size_t i = 0; i < chunk_count; ++i) stats[i] = {};
    }
    return false;
}

bool EdfReportProvider::for_each_compatible_series_plot_batch(
    const ReportProviderChunk *chunks,
    size_t chunk_count,
    const EdfReportSeriesPlotConfig *configs,
    bool *selected,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderSeriesReadStats *stats,
    EdfReportSeriesBatchPlotCallback callback,
    void *context) const {
    (void)chunks;
    (void)configs;
    (void)sessions;
    (void)session_count;
    (void)callback;
    (void)context;
    if (selected) {
        for (size_t i = 0; i < chunk_count; ++i) selected[i] = false;
    }
    if (stats) {
        for (size_t i = 0; i < chunk_count; ++i) stats[i] = {};
    }
    return false;
}
#endif

bool EdfReportDataProvider::chunk_extent(ReportStoreChunkKind kind,
                                         const ReportSourceDef &source,
                                         const char *name,
                                         int64_t night_start_ms,
                                         int64_t start_ms,
                                         int64_t end_ms,
                                         ReportProviderChunkExtent &out) const {
    out = {};
    ReportSignalId signal = ReportSignalId::Flow;
    if (kind == ReportStoreChunkKind::Series &&
        !signal_for_store_name(name, signal)) {
        return false;
    }
    EdfProviderExtentContext ctx;
    if (!for_each_chunk(kind,
                        source,
                        signal,
                        name,
                        night_start_ms,
                        start_ms,
                        end_ms,
                        remember_edf_provider_extent,
                        &ctx)) {
        return false;
    }
    if (!ctx.found) return false;
    out.found = true;
    out.min_start_ms = ctx.min_start_ms;
    out.max_end_ms = ctx.max_end_ms;
    return true;
}

bool EdfReportDataProvider::latest_cached_end(
    const ReportSourceDef &source,
    int64_t night_start_ms,
    int64_t start_ms,
    int64_t end_ms,
    int64_t &out_end_ms) const {
    out_end_ms = 0;

    if (source.id == ReportSourceId::RespiratoryEvents) {
        ReportProviderChunkExtent extent;
        if (!chunk_extent(ReportStoreChunkKind::Events,
                          source,
                          report_source_spool_type(source.id),
                          night_start_ms,
                          start_ms,
                          end_ms,
                          extent)) {
            return false;
        }
        out_end_ms = extent.max_end_ms;
        return true;
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    bool matched = false;
    int64_t earliest_latest_end = 0;
    for (size_t i = 0; i < signal_count; ++i) {
        if (signals[i].preferred_source != source.id &&
            signals[i].fallback_source != source.id) {
            continue;
        }
        ReportProviderChunkExtent extent;
        if (!chunk_extent(ReportStoreChunkKind::Series,
                          source,
                          signals[i].store_name,
                          night_start_ms,
                          start_ms,
                          end_ms,
                          extent)) {
            return false;
        }
        if (!matched || extent.max_end_ms < earliest_latest_end) {
            earliest_latest_end = extent.max_end_ms;
        }
        matched = true;
    }
    if (!matched) return false;
    out_end_ms = earliest_latest_end;
    return true;
}

}  // namespace aircannect
