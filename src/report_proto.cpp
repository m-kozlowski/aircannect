#include "report_proto.h"

#include <math.h>
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

int16_t clamp_i16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return static_cast<int16_t>(value);
}

int16_t summary_scaled_digital(uint64_t value, float multiplier) {
    const float scaled = static_cast<float>(value) * multiplier;
    return clamp_i16(static_cast<int32_t>(lroundf(scaled)));
}

bool set_summary_str_sample(ReportSummaryRecord &record,
                            size_t signal_index,
                            int16_t digital) {
    if (signal_index < AC_REPORT_SUMMARY_STR_FIRST_SIGNAL ||
        signal_index > AC_REPORT_SUMMARY_STR_LAST_SIGNAL) {
        return false;
    }
    const size_t slot = signal_index - AC_REPORT_SUMMARY_STR_FIRST_SIGNAL;
    if (slot >= AC_REPORT_SUMMARY_STR_VALUE_COUNT || slot >= 64) {
        return false;
    }
    record.str_summary_digital[slot] = digital;
    record.str_summary_mask |= 1ULL << slot;
    return true;
}

void set_summary_str_raw(ReportSummaryRecord &record,
                         size_t signal_index,
                         uint64_t value) {
    (void)set_summary_str_sample(record,
                                 signal_index,
                                 clamp_i16(static_cast<int32_t>(value)));
}

void set_summary_str_scaled(ReportSummaryRecord &record,
                            size_t signal_index,
                            uint64_t value,
                            float multiplier) {
    (void)set_summary_str_sample(record,
                                 signal_index,
                                 summary_scaled_digital(value, multiplier));
}

struct SummaryScalarStrMap {
    uint32_t field = 0;
    size_t signal_index = 0;
};

static constexpr SummaryScalarStrMap SUMMARY_SCALAR_STR_MAP[] = {
    {35, 76},   // ZHT Summary-TubeConnected
    {34, 77},   // HUC Summary-HumidifierConnected
    {17, 91},   // SAU SpO2Thresh
    {18, 92},   // VSR SpontTrig%
    {19, 93},   // VCR SpontCyc%
    {7, 125},   // AHI
    {9, 126},   // HSC HypopneaIndex
    {8, 127},   // ASC ApneaIndex
    {10, 128},  // CSC ObstructiveApneaIndex
    {11, 129},  // OSC CentralApneaIndex
    {12, 130},  // USC UnknownApneaIndex
    {13, 131},  // RCC ReraIndex
    {16, 132},  // CSD CSR
};

struct SummaryMetricStrMap {
    uint32_t field = 0;
    uint32_t subfield = 0;
    size_t signal_index = 0;
    float multiplier = 1.0f;
};

static constexpr SummaryMetricStrMap SUMMARY_METRIC_STR_MAP[] = {
    {36, 3, 78, 2.0f},    // BP9
    {36, 1, 79, 2.0f},    // BP5
    {37, 3, 80, 0.2f},    // R95
    {37, 1, 81, 0.2f},    // RFM
    {38, 2, 82, 0.2f},    // BFM
    {29, 2, 83, 10.0f},   // AUM
    {30, 2, 84, 10.0f},   // HHE
    {31, 2, 85, 10.0f},   // HTE
    {33, 2, 86, 10.0f},   // AHM
    {32, 2, 87, 10.0f},   // APM
    {28, 2, 88, 1.0f},    // SOM
    {28, 3, 89, 1.0f},    // SO9
    {28, 4, 90, 1.0f},    // SOX
    {21, 2, 94, 2.0f},    // MSP
    {21, 3, 95, 2.0f},    // PM9
    {21, 4, 96, 2.0f},    // PMA
    {15, 2, 97, 2.0f},    // PIM
    {15, 3, 98, 2.0f},    // PI9
    {15, 4, 99, 2.0f},    // PIA
    {20, 2, 100, 2.0f},   // PEM
    {20, 3, 101, 2.0f},   // PE9
    {20, 4, 102, 2.0f},   // PEA
    {14, 2, 103, 2.0f},   // LKM
    {14, 4, 104, 2.0f},   // LK9
    {14, 3, 105, 2.0f},   // LK7
    {14, 5, 106, 2.0f},   // LMX
    {23, 2, 107, 8.0f},   // VTM
    {23, 3, 108, 8.0f},   // VT9
    {23, 4, 109, 8.0f},   // VTA
    {25, 2, 110, 5.0f},   // RRM
    {25, 3, 111, 5.0f},   // RR9
    {25, 4, 112, 5.0f},   // RRA
    {22, 2, 113, 2.0f},   // TVM
    {22, 3, 114, 2.0f},   // TV9
    {22, 4, 115, 2.0f},   // TVA
    {24, 2, 116, 1.0f},   // VAM
    {24, 3, 117, 1.0f},   // VA9
    {24, 4, 118, 1.0f},   // VAA
    {27, 2, 119, 1.0f},   // IEM
    {27, 3, 120, 1.0f},   // IE9
    {27, 4, 121, 1.0f},   // IEA
    {26, 2, 122, 1.0f},   // ISM
    {26, 3, 123, 1.0f},   // IS9
    {26, 4, 124, 1.0f},   // ISA
};

void apply_summary_scalar_str_field(ReportSummaryRecord &record,
                                    uint32_t field_id,
                                    uint64_t value) {
    for (const SummaryScalarStrMap &map : SUMMARY_SCALAR_STR_MAP) {
        if (map.field == field_id) {
            set_summary_str_raw(record, map.signal_index, value);
            return;
        }
    }
}

void apply_summary_metric_str_field(ReportSummaryRecord &record,
                                    uint32_t field_id,
                                    uint32_t subfield_id,
                                    uint64_t value) {
    for (const SummaryMetricStrMap &map : SUMMARY_METRIC_STR_MAP) {
        if (map.field == field_id && map.subfield == subfield_id) {
            set_summary_str_scaled(record,
                                   map.signal_index,
                                   value,
                                   map.multiplier);
            return;
        }
    }
}

void parse_summary_metric_fields(uint32_t field_id,
                                 const uint8_t *data,
                                 size_t len,
                                 ReportSummaryRecord &record) {
    size_t index = 0;
    ReportProtoField field;
    while (report_proto_next(data, len, index, field)) {
        if (field.wire != 0) continue;
        apply_summary_metric_str_field(record,
                                       field_id,
                                       field.field,
                                       field.value);
    }
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

bool report_summary_str_sample(const ReportSummaryRecord &record,
                               size_t signal_index,
                               int16_t &out) {
    if (signal_index < AC_REPORT_SUMMARY_STR_FIRST_SIGNAL ||
        signal_index > AC_REPORT_SUMMARY_STR_LAST_SIGNAL) {
        return false;
    }
    const size_t slot = signal_index - AC_REPORT_SUMMARY_STR_FIRST_SIGNAL;
    if (slot >= AC_REPORT_SUMMARY_STR_VALUE_COUNT || slot >= 64) {
        return false;
    }
    if ((record.str_summary_mask & (1ULL << slot)) == 0) return false;
    out = record.str_summary_digital[slot];
    return true;
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
        if (field.wire == 2) {
            if (field.field == 6) {
                parse_summary_sessions(field.data, field.len, out);
            } else {
                parse_summary_metric_fields(field.field,
                                            field.data,
                                            field.len,
                                            out);
            }
            continue;
        }
        if (field.wire != 0) continue;
        apply_summary_scalar_str_field(out, field.field, field.value);
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
