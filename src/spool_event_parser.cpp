#include "spool_event_parser.h"

#include <limits.h>
#include <stdio.h>

#include "report_proto.h"

namespace aircannect {
namespace {

void set_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message ? message : "");
}

bool parse_event_record(const uint8_t *data,
                        size_t len,
                        SpoolEventRecord &record) {
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

    if (!has_type || !has_start || !has_end || event_type > UINT16_MAX ||
        start > INT64_MAX || end > INT64_MAX) {
        return false;
    }
    if (!has_duration) duration = end > start ? end - start : 0;
    if (duration > INT32_MAX) return false;

    record = {};
    record.type = static_cast<uint16_t>(event_type);
    record.start_ms = static_cast<int64_t>(start);
    record.end_ms = static_cast<int64_t>(end);
    record.duration_ms = static_cast<int32_t>(duration);
    return true;
}

bool walk_event_records(const uint8_t *data,
                        size_t len,
                        uint8_t depth,
                        SpoolEventRecordCallback callback,
                        void *context,
                        SpoolEventParseStats *stats) {
    size_t index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;
        if (field.wire != 2) continue;

        if (depth < 2) {
            if (!walk_event_records(field.data, field.len, depth + 1,
                                    callback, context, stats)) {
                return false;
            }
            continue;
        }
        if (depth != 2 || field.field != 1) continue;

        if (stats) stats->records++;
        SpoolEventRecord record;
        if (!parse_event_record(field.data, field.len, record)) {
            if (stats) stats->malformed_records++;
            continue;
        }
        if (!callback(context, record)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool spool_result_valid_for_type(const ReportSpoolResult &result,
                                 const char *expected_type,
                                 char *error,
                                 size_t error_len) {
    if (!expected_type || !expected_type[0] ||
        result.spool_type != expected_type) {
        set_error(error, error_len, "wrong_spool_type");
        return false;
    }
    if (!result.complete) {
        set_error(error, error_len, "spool_incomplete");
        return false;
    }
    if (result.truncated) {
        set_error(error, error_len, "spool_truncated");
        return false;
    }
    if (!result.sha_ok) {
        set_error(error, error_len, "spool_hash_failed");
        return false;
    }
    if (!result.payload.data() || result.payload.size() == 0) {
        set_error(error, error_len, "spool_empty");
        return false;
    }

    set_error(error, error_len, "");
    return true;
}

bool spool_parse_event_records(const uint8_t *data,
                               size_t len,
                               SpoolEventRecordCallback callback,
                               void *context,
                               SpoolEventParseStats *stats) {
    if ((!data && len != 0) || !callback) return false;
    if (stats) *stats = {};
    return walk_event_records(data, len, 0, callback, context, stats);
}

}  // namespace aircannect
