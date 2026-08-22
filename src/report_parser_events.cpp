#include "report_parser.h"

#include <stdio.h>

#include "report_records.h"
#include "spool_event_parser.h"

namespace aircannect {
namespace {

void set_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message ? message : "");
}

bool is_event_source(ReportSourceId source) {
    return source == ReportSourceId::UsageEvents ||
           source == ReportSourceId::RespiratoryEvents;
}

struct EventParseContext {
    ReportSpoolBuffer payload;
    uint32_t record_count = 0;
    int64_t min_start_ms = INT64_MAX;
    int64_t max_end_ms = 0;
};

bool append_event_record(EventParseContext &context,
                         const ReportEventRecord &record) {
    if (!report_append_event_record(context.payload, record)) return false;

    context.record_count++;
    if (record.start_ms < context.min_start_ms) {
        context.min_start_ms = record.start_ms;
    }

    const int64_t event_end =
        record.start_ms + (record.duration_ms > 0 ? record.duration_ms : 1);
    if (event_end > context.max_end_ms) context.max_end_ms = event_end;
    return true;
}

bool capture_event_record(void *context, const SpoolEventRecord &spool) {
    EventParseContext *parsed = static_cast<EventParseContext *>(context);
    if (!parsed) return false;

    ReportEventRecord record;
    record.start_ms = spool.start_ms;
    record.duration_ms = spool.duration_ms;
    record.code = spool.type;
    return append_event_record(*parsed, record);
}

}  // namespace

bool report_parse_event_spool(const ReportSpoolResult &result,
                              ReportSourceId source,
                              ReportParsedChunkCallback callback,
                              void *context,
                              char *error,
                              size_t error_len) {
    if (!is_event_source(source)) {
        set_error(error, error_len, "not_event_source");
        return false;
    }
    if (!callback) {
        set_error(error, error_len, "missing_chunk_callback");
        return false;
    }
    if (!report_validate_spool_for_source(result, source, error, error_len)) {
        return false;
    }

    EventParseContext parsed;
    if (!spool_parse_event_records(result.payload.data(),
                                   result.payload.size(),
                                   capture_event_record,
                                   &parsed)) {
        set_error(error, error_len, "event_parse_failed");
        return false;
    }
    if (!parsed.record_count) {
        set_error(error, error_len, "");
        return true;
    }

    const ReportSourceDef *def = report_source_def(source);
    ReportParsedChunk chunk;
    chunk.source = source;
    chunk.kind = ReportParsedChunkKind::Events;
    chunk.name = def ? def->spool_type : "";
    chunk.start_ms = parsed.min_start_ms;
    chunk.end_ms = parsed.max_end_ms;
    chunk.payload_schema = REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1;
    chunk.record_count = parsed.record_count;
    chunk.payload = parsed.payload.data();
    chunk.payload_len = parsed.payload.size();
    if (!callback(context, chunk)) {
        if (!error || !error[0]) {
            set_error(error, error_len, "event_chunk_rejected");
        }
        return false;
    }

    set_error(error, error_len, "");
    return true;
}

}  // namespace aircannect
