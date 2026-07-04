#include "report_manager.h"

#include <stdio.h>
#include <string>
#include <string.h>

#include "report_parser.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

struct ChunkWriteContext {
    ReportManager *manager = nullptr;
    ReportSourceId source = ReportSourceId::Summary;
    char *error = nullptr;
    size_t error_len = 0;
};

}  // namespace

bool ReportManager::write_parsed_chunk(void *context,
                                       const ReportParsedChunk &chunk) {
    ChunkWriteContext *ctx = static_cast<ChunkWriteContext *>(context);
    if (!ctx || !ctx->manager || !chunk.name || !chunk.name[0] ||
        !chunk.payload || chunk.payload_schema == 0 ||
        chunk.record_count == 0 ||
        chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) {
        if (ctx && ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s", "bad_cache_chunk");
        }

        return false;
    }

    if (!report_source_spool_type(chunk.source)) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s", "bad_cache_source");
        }

        return false;
    }

    if (chunk.kind == ReportStoreChunkKind::Series &&
        chunk.payload_schema != REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error,
                     ctx->error_len,
                     "%s",
                     "bad_series_payload_schema");
        }

        return false;
    }

    if (chunk.kind == ReportStoreChunkKind::Events &&
        chunk.payload_schema != REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error,
                     ctx->error_len,
                     "%s",
                     "bad_event_payload_schema");
        }

        return false;
    }

    if (!ctx->manager->buffer_parsed_chunk(chunk)) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            const std::string &detail = ctx->manager->cache_status_.error;
            snprintf(ctx->error,
                     ctx->error_len,
                     "%s",
                     detail.empty() ? "cache_chunk_store_failed"
                                    : detail.c_str());
        }

        return false;
    }

    return true;
}

bool ReportManager::store_cache_round(ReportSpoolResult &result) {
    const ReportSourceId source = cache_plan_[cache_source_index_].source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    ChunkWriteContext context;
    context.manager = this;
    context.source = source;

    char error[64] = {};
    context.error = error;
    context.error_len = sizeof(error);

    bool parsed = false;
    switch (source) {
        case ReportSourceId::UsageEvents:
        case ReportSourceId::RespiratoryEvents:
            parsed = report_parse_event_spool(result,
                                              source,
                                              write_parsed_chunk,
                                              &context,
                                              error,
                                              sizeof(error));
            break;

        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            parsed = report_parse_series_spool(result,
                                               source,
                                               write_parsed_chunk,
                                               &context,
                                               error,
                                               sizeof(error));
            break;

        default:
            snprintf(error, sizeof(error), "%s", "unsupported_cache_source");
            parsed = false;
            break;
    }

    if (!parsed) {
        // An empty source spool is not a fetch failure: a session can hold zero
        // events, or a sampled source can have aged out.
        if (strcmp(error, "spool_empty") == 0) return true;

        fail_cache_fetch(error[0] ? error : "cache_parse_failed");
        return false;
    }

    return true;
}

}  // namespace aircannect
