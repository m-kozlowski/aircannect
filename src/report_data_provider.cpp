#include "report_data_provider.h"

#include <stddef.h>
#include <string.h>

namespace aircannect {
namespace {

bool valid_source(const ReportSourceDef &source) {
    return source.spool_type && source.spool_type[0] &&
           source.parser_schema != 0;
}

struct ChunkExtentContext {
    bool found = false;
    int64_t min_start_ms = 0;
    int64_t max_end_ms = 0;
};

struct ProviderChunkContext {
    const SpoolReportProvider *provider = nullptr;
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Flow;
    const char *name = nullptr;
    ReportProviderChunkCallback callback = nullptr;
    void *context = nullptr;
};

bool remember_chunk_extent(void *context, const ReportStoreChunkInfo &chunk) {
    ChunkExtentContext *ctx = static_cast<ChunkExtentContext *>(context);
    if (!ctx) return false;
    if (!ctx->found || chunk.key.start_ms < ctx->min_start_ms) {
        ctx->min_start_ms = chunk.key.start_ms;
    }
    if (!ctx->found || chunk.key.end_ms > ctx->max_end_ms) {
        ctx->max_end_ms = chunk.key.end_ms;
    }
    ctx->found = true;
    return true;
}

bool emit_provider_chunk(void *context, const ReportStoreChunkInfo &chunk) {
    ProviderChunkContext *ctx = static_cast<ProviderChunkContext *>(context);
    if (!ctx || !ctx->provider || !ctx->callback) return false;

    ReportProviderChunk out;
    out.ref.provider = ctx->provider->provider_id();
    out.kind = ctx->kind;
    out.source = ctx->source;
    out.signal = ctx->signal;
    out.name = ctx->name;
    out.start_ms = chunk.key.start_ms;
    out.end_ms = chunk.key.end_ms;
    out.payload_schema = chunk.meta.payload_schema;
    out.record_count = chunk.meta.record_count;
    out.payload_len = chunk.payload_len;
    return ctx->callback(ctx->context, out);
}

struct LatestChunkEndContext {
    bool found = false;
    int64_t latest_end_ms = 0;
};

bool remember_latest_chunk_end(void *context,
                               const ReportStoreChunkInfo &chunk) {
    LatestChunkEndContext *ctx =
        static_cast<LatestChunkEndContext *>(context);
    if (!ctx) return false;
    if (!ctx->found || chunk.key.end_ms > ctx->latest_end_ms) {
        ctx->found = true;
        ctx->latest_end_ms = chunk.key.end_ms;
    }
    return true;
}

bool latest_chunk_end_for_name(const SpoolReportProvider &provider,
                               ReportStoreChunkKind kind,
                               const ReportSourceDef &source,
                               const char *name,
                               int64_t night_start_ms,
                               int64_t start_ms,
                               int64_t end_ms,
                               int64_t &out_end_ms) {
    LatestChunkEndContext ctx;
    if (!ReportStore::for_each_chunk(kind,
                                     source.spool_type,
                                     name,
                                     night_start_ms,
                                     start_ms,
                                     end_ms,
                                     remember_latest_chunk_end,
                                     &ctx)) {
        return false;
    }
    if (!ctx.found) return false;
    out_end_ms = ctx.latest_end_ms;
    return true;
}

bool source_uses_signal(const ReportSignalDef &signal,
                        ReportSourceId source) {
    return signal.preferred_source == source || signal.fallback_source == source;
}

}  // namespace

bool SpoolReportProvider::coverage_complete(const ReportSourceDef &source,
                                            int64_t start_ms,
                                            int64_t end_ms) const {
    if (!valid_source(source)) return false;
    return ReportStore::coverage_complete(source.spool_type,
                                          start_ms,
                                          end_ms,
                                          source.parser_schema);
}

bool SpoolReportProvider::coverage_first_missing(
    const ReportSourceDef &source,
    int64_t start_ms,
    int64_t end_ms,
    int64_t &missing_ms) const {
    missing_ms = start_ms;
    if (!valid_source(source)) return false;
    return ReportStore::coverage_first_missing(source.spool_type,
                                               start_ms,
                                               end_ms,
                                               source.parser_schema,
                                               missing_ms);
}

bool SpoolReportProvider::for_each_chunk(
    ReportStoreChunkKind kind,
    const ReportSourceDef &source,
    ReportSignalId signal,
    const char *name,
    int64_t night_start_ms,
    int64_t start_ms,
    int64_t end_ms,
    ReportProviderChunkCallback callback,
    void *context) const {
    if (!valid_source(source) || !name || !name[0] || !callback) return false;
    ProviderChunkContext ctx;
    ctx.provider = this;
    ctx.kind = kind;
    ctx.source = source.id;
    ctx.signal = signal;
    ctx.name = name;
    ctx.callback = callback;
    ctx.context = context;
    return ReportStore::for_each_chunk(kind,
                                       source.spool_type,
                                       name,
                                       night_start_ms,
                                       start_ms,
                                       end_ms,
                                       emit_provider_chunk,
                                       &ctx);
}

bool SpoolReportProvider::read_chunk(const ReportProviderChunk &chunk,
                                     int64_t night_start_ms,
                                     ReportStoreChunkMeta &meta,
                                     ReportSpoolBuffer &payload) const {
    meta = {};
    payload.clear();
    const ReportSourceDef *source = report_source_def(chunk.source);
    if (!source || !valid_source(*source) || !chunk.name || !chunk.name[0]) {
        return false;
    }
    ReportStoreChunkKey key;
    key.kind = chunk.kind;
    key.source = source->spool_type;
    key.name = chunk.name;
    key.start_ms = chunk.start_ms;
    key.end_ms = chunk.end_ms;
    key.night_start_ms = night_start_ms;
    return ReportStore::read_chunk(key, meta, payload);
}

bool SpoolReportProvider::chunk_extent(
    ReportStoreChunkKind kind,
    const ReportSourceDef &source,
    const char *name,
    int64_t night_start_ms,
    int64_t start_ms,
    int64_t end_ms,
    ReportProviderChunkExtent &out) const {
    out = {};
    ChunkExtentContext ctx;
    if (!valid_source(source) || !name || !name[0]) return false;
    if (!ReportStore::for_each_chunk(kind,
                                     source.spool_type,
                                     name,
                                     night_start_ms,
                                     start_ms,
                                     end_ms,
                                     remember_chunk_extent,
                                     &ctx)) {
        return false;
    }
    if (!ctx.found) return false;
    out.found = true;
    out.min_start_ms = ctx.min_start_ms;
    out.max_end_ms = ctx.max_end_ms;
    return true;
}

bool SpoolReportProvider::latest_cached_end(
    const ReportSourceDef &source,
    int64_t night_start_ms,
    int64_t start_ms,
    int64_t end_ms,
    int64_t &out_end_ms) const {
    out_end_ms = 0;
    if (!valid_source(source)) return false;

    if (source.id == ReportSourceId::UsageEvents ||
        source.id == ReportSourceId::RespiratoryEvents) {
        return latest_chunk_end_for_name(*this,
                                         ReportStoreChunkKind::Events,
                                         source,
                                         source.spool_type,
                                         night_start_ms,
                                         start_ms,
                                         end_ms,
                                         out_end_ms);
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    bool matched = false;
    int64_t earliest_latest_end = 0;
    for (size_t i = 0; i < signal_count; ++i) {
        if (!source_uses_signal(signals[i], source.id)) continue;
        int64_t signal_end = 0;
        if (!latest_chunk_end_for_name(*this,
                                       ReportStoreChunkKind::Series,
                                       source,
                                       signals[i].store_name,
                                       night_start_ms,
                                       start_ms,
                                       end_ms,
                                       signal_end)) {
            return false;
        }
        if (!matched || signal_end < earliest_latest_end) {
            earliest_latest_end = signal_end;
        }
        matched = true;
    }
    if (!matched) return false;
    out_end_ms = earliest_latest_end;
    return true;
}

}  // namespace aircannect
