#include "report_proto.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {
namespace {

void set_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message ? message : "");
}

float summary_index_value(uint64_t value) {
    return static_cast<float>(value) / 10.0f;
}

}  // namespace

bool report_proto_read_varint(const uint8_t *data,
                              size_t len,
                              size_t &index,
                              uint64_t &out) {
    uint64_t value = 0;
    uint8_t shift = 0;
    while (index < len && shift < 64) {
        const uint8_t byte = data[index++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool report_proto_next(const uint8_t *data,
                       size_t len,
                       size_t &index,
                       ReportProtoField &out) {
    uint64_t key = 0;
    if (!report_proto_read_varint(data, len, index, key)) return false;
    out = {};
    out.field = static_cast<uint32_t>(key >> 3);
    out.wire = static_cast<uint8_t>(key & 0x07);
    switch (out.wire) {
        case 0:
            return report_proto_read_varint(data, len, index, out.value);
        case 1:
            if (len - index < 8) return false;
            out.data = data + index;
            out.len = 8;
            index += 8;
            return true;
        case 2: {
            uint64_t field_len = 0;
            if (!report_proto_read_varint(data,
                                          len,
                                          index,
                                          field_len)) {
                return false;
            }
            if (field_len > len - index) return false;
            out.data = data + index;
            out.len = static_cast<size_t>(field_len);
            index += out.len;
            return true;
        }
        case 5:
            if (len - index < 4) return false;
            out.data = data + index;
            out.len = 4;
            index += 4;
            return true;
        default:
            return false;
    }
}

bool report_proto_all_length_fields(const uint8_t *data,
                                    size_t len,
                                    uint32_t field_id) {
    size_t index = 0;
    bool any = false;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;
        if (field.field != field_id || field.wire != 2) return false;
        any = true;
    }
    return any;
}

namespace {

void append_summary_session(ReportSummaryRecord &record,
                            uint64_t start_ms,
                            uint32_t duration_min) {
    if (!start_ms || !duration_min ||
        record.session_interval_count >= AC_REPORT_SUMMARY_SESSION_MAX) {
        return;
    }

    ReportSummarySession &session =
        record.sessions[record.session_interval_count++];
    session.start_ms = start_ms;
    session.duration_min = duration_min;
}

void parse_summary_session_leaf(const uint8_t *data,
                                size_t len,
                                ReportSummaryRecord &record) {
    uint64_t start_ms = 0;
    uint32_t duration_min = 0;
    size_t index = 0;
    ReportProtoField field;
    while (report_proto_next(data, len, index, field)) {
        if (field.wire != 0) continue;
        switch (field.field) {
            case 1:
                start_ms = field.value;
                break;
            case 2:
                duration_min = static_cast<uint32_t>(field.value);
                break;
            default:
                break;
        }
    }
    append_summary_session(record, start_ms, duration_min);
}

void parse_summary_sessions(const uint8_t *data,
                            size_t len,
                            ReportSummaryRecord &record) {
    size_t index = 0;
    bool parsed_wrapped = false;
    ReportProtoField field;
    while (report_proto_next(data, len, index, field)) {
        if (field.field == 1 && field.wire == 2) {
            parse_summary_session_leaf(field.data, field.len, record);
            parsed_wrapped = true;
        }
    }
    if (!parsed_wrapped) {
        parse_summary_session_leaf(data, len, record);
    }
}

bool parse_summary_record(const uint8_t *record,
                          size_t len,
                          ReportSummaryRecord &out) {
    out = {};
    size_t index = 0;
    ReportProtoField field;
    while (report_proto_next(record, len, index, field)) {
        if (field.field == 6 && field.wire == 2) {
            parse_summary_sessions(field.data, field.len, out);
            continue;
        }
        if (field.wire != 0) continue;
        switch (field.field) {
            case 2:
                out.start_ms = field.value;
                break;
            case 3:
                out.end_ms = field.value;
                break;
            case 4:
                out.has_tz_offset_min = true;
                out.tz_offset_min = static_cast<int32_t>(field.value);
                break;
            case 5:
                out.duration_min = static_cast<uint32_t>(field.value);
                break;
            case 7:
                out.has_ahi = true;
                out.ahi = summary_index_value(field.value);
                break;
            case 8:
                out.has_apnea_index = true;
                out.apnea_index = summary_index_value(field.value);
                break;
            case 9:
                out.has_hypopnea_index = true;
                out.hypopnea_index = summary_index_value(field.value);
                break;
            case 10:
                out.has_oa_index = true;
                out.oa_index = summary_index_value(field.value);
                break;
            case 11:
                out.has_ca_index = true;
                out.ca_index = summary_index_value(field.value);
                break;
            case 12:
                out.has_ua_index = true;
                out.ua_index = summary_index_value(field.value);
                break;
            case 13:
                out.has_rera_index = true;
                out.rera_index = summary_index_value(field.value);
                break;
            case 39:
                out.has_session_count = true;
                out.session_count = static_cast<uint32_t>(field.value);
                break;
            default:
                break;
        }
    }
    if (!out.start_ms) return false;
    if (!out.end_ms) out.end_ms = out.start_ms + 24ULL * 60ULL * 60ULL * 1000ULL;
    if (!out.has_session_count && out.session_interval_count > 0) {
        out.has_session_count = true;
        out.session_count = out.session_interval_count;
    }
    if (!out.duration_min && out.session_interval_count > 0) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < out.session_interval_count; ++i) {
            total += out.sessions[i].duration_min;
        }
        out.duration_min = total;
    }
    out.valid = true;
    return true;
}

bool emit_summary_record(const uint8_t *record,
                         size_t len,
                         ReportSummaryRecordCallback callback,
                         void *context) {
    ReportSummaryRecord parsed;
    if (!parse_summary_record(record, len, parsed)) return true;
    return callback ? callback(context, parsed) : true;
}

}  // namespace

bool report_parse_summary_records(const uint8_t *data,
                                  size_t len,
                                  ReportSummaryRecordCallback callback,
                                  void *context,
                                  char *error,
                                  size_t error_len) {
    if (!data || !len) {
        set_error(error, error_len, "empty_summary");
        return false;
    }
    if (!callback) {
        set_error(error, error_len, "missing_callback");
        return false;
    }

    if (report_proto_all_length_fields(data, len, 2)) {
        size_t index = 0;
        ReportProtoField field;
        while (report_proto_next(data, len, index, field)) {
            if (!emit_summary_record(field.data,
                                     field.len,
                                     callback,
                                     context)) {
                set_error(error, error_len, "summary_record_rejected");
                return false;
            }
        }
        return true;
    }

    if (emit_summary_record(data, len, callback, context)) return true;
    set_error(error, error_len, "summary_record_rejected");
    return false;
}

}  // namespace aircannect
