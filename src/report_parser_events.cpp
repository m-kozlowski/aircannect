#include "report_parser.h"

#include <limits.h>
#include <stdio.h>

#include "report_records.h"

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

bool parse_event_record(const uint8_t *data,
                        size_t len,
                        ReportEventRecord &record) {
    bool has_type = false;
    bool has_start = false;
    bool has_end = false;
    bool has_duration = false;
    uint64_t event_type = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t duration = 0;

    size_t index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;
        if (field.wire != 0) continue;

        switch (field.field) {
            case 1:
                has_type = true;
                event_type = field.value;
                break;
            case 2:
                has_start = true;
                start = field.value;
                break;
            case 3:
                has_end = true;
                end = field.value;
                break;
            case 4:
                has_duration = true;
                duration = field.value;
                break;
            default:
                break;
        }
    }

    if (!has_type || !has_start || !has_end ||
        event_type > UINT16_MAX || start > INT64_MAX || end > INT64_MAX) {
        return false;
    }
    if (!has_duration) {
        duration = end > start ? end - start : 0;
    }
    if (duration > INT32_MAX) return false;

    record = {};
    record.start_ms = static_cast<int64_t>(start);
    record.duration_ms = static_cast<int32_t>(duration);
    record.code = static_cast<uint16_t>(event_type);
    return true;
}

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

bool walk_event_records(const uint8_t *data,
                        size_t len,
                        uint8_t depth,
                        EventParseContext &context) {
    size_t index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;
        if (field.wire != 2) continue;

        if (depth < 2) {
            if (!walk_event_records(field.data,
                                    field.len,
                                    depth + 1,
                                    context)) {
                return false;
            }
        } else if (depth == 2 && field.field == 1) {
            ReportEventRecord record;
            if (parse_event_record(field.data, field.len, record)) {
                if (!append_event_record(context, record)) return false;
            }
        }
    }
    return true;
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
    if (!walk_event_records(result.payload.data(),
                            result.payload.size(),
                            0,
                            parsed)) {
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
