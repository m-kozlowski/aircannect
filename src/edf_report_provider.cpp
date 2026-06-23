#include "edf_report_provider.h"

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
            : REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V1;
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

}  // namespace aircannect
