#include "report_proto.h"

#include <stdio.h>

namespace aircannect {
namespace {

void set_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message ? message : "");
}

float summary_index_value(uint64_t value) {
    return static_cast<float>(value) / 10.0f;
}

bool set_summary_field_value(ReportSummaryRecord &record,
                             ReportSummaryField field,
                             uint64_t value) {
    const size_t index = static_cast<size_t>(field);
    if (index >= AC_REPORT_SUMMARY_FIELD_COUNT || index >= 64) return false;

    record.summary_field_values[index] =
        value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
    record.summary_field_mask |= 1ULL << index;
    return true;
}

struct SummaryScalarFieldMap {
    uint32_t field = 0;
    ReportSummaryField summary = ReportSummaryField::Count;
};

static constexpr SummaryScalarFieldMap SUMMARY_SCALAR_FIELD_MAP[] = {
    {34, ReportSummaryField::TubeConnected},
    {35, ReportSummaryField::HumidifierConnected},
    {17, ReportSummaryField::Spo2ThresholdMinutes},
    {18, ReportSummaryField::SpontaneousTriggerPercent},
    {19, ReportSummaryField::SpontaneousCyclePercent},
    {7, ReportSummaryField::Ahi},
    {9, ReportSummaryField::HypopneaIndex},
    {8, ReportSummaryField::ApneaIndex},
    {10, ReportSummaryField::ObstructiveApneaIndex},
    {11, ReportSummaryField::CentralApneaIndex},
    {12, ReportSummaryField::UnknownApneaIndex},
    {13, ReportSummaryField::ReraIndex},
    {16, ReportSummaryField::Csr},
};

struct SummaryMetricFieldMap {
    uint32_t field = 0;
    uint32_t subfield = 0;
    ReportSummaryField summary = ReportSummaryField::Count;
};

static constexpr SummaryMetricFieldMap SUMMARY_METRIC_FIELD_MAP[] = {
    {36, 3, ReportSummaryField::BlowPressure95},
    {36, 1, ReportSummaryField::BlowPressure5},
    {37, 3, ReportSummaryField::Flow95},
    {37, 1, ReportSummaryField::Flow5},
    {38, 2, ReportSummaryField::BlowerFlow50},
    {29, 2, ReportSummaryField::AmbientHumidity50},
    {30, 2, ReportSummaryField::HumidifierTemperature50},
    {31, 2, ReportSummaryField::HeatedTubeTemperature50},
    {33, 2, ReportSummaryField::HeatedTubePower50},
    {32, 2, ReportSummaryField::HumidifierPower50},
    {28, 2, ReportSummaryField::Spo2Median},
    {28, 3, ReportSummaryField::Spo2_95},
    {28, 4, ReportSummaryField::Spo2Max},
    {21, 2, ReportSummaryField::MaskPressureMedian},
    {21, 3, ReportSummaryField::MaskPressure95},
    {21, 4, ReportSummaryField::MaskPressureMax},
    {15, 2, ReportSummaryField::TargetIpapMedian},
    {15, 3, ReportSummaryField::TargetIpap95},
    {15, 4, ReportSummaryField::TargetIpapMax},
    {20, 2, ReportSummaryField::TargetEpapMedian},
    {20, 3, ReportSummaryField::TargetEpap95},
    {20, 4, ReportSummaryField::TargetEpapMax},
    {14, 2, ReportSummaryField::LeakMedian},
    {14, 4, ReportSummaryField::Leak95},
    {14, 3, ReportSummaryField::Leak70},
    {14, 5, ReportSummaryField::LeakMax},
    {23, 2, ReportSummaryField::MinuteVentMedian},
    {23, 3, ReportSummaryField::MinuteVent95},
    {23, 4, ReportSummaryField::MinuteVentMax},
    {25, 2, ReportSummaryField::RespiratoryRateMedian},
    {25, 3, ReportSummaryField::RespiratoryRate95},
    {25, 4, ReportSummaryField::RespiratoryRateMax},
    {22, 2, ReportSummaryField::TidalVolumeMedian},
    {22, 3, ReportSummaryField::TidalVolume95},
    {22, 4, ReportSummaryField::TidalVolumeMax},
    {24, 2, ReportSummaryField::TargetVentMedian},
    {24, 3, ReportSummaryField::TargetVent95},
    {24, 4, ReportSummaryField::TargetVentMax},
    {27, 2, ReportSummaryField::IeRatioMedian},
    {27, 3, ReportSummaryField::IeRatio95},
    {27, 4, ReportSummaryField::IeRatioMax},
    {26, 2, ReportSummaryField::InspirationTimeMedian},
    {26, 3, ReportSummaryField::InspirationTime95},
    {26, 4, ReportSummaryField::InspirationTimeMax},
};

void apply_summary_scalar_field(ReportSummaryRecord &record,
                                uint32_t field_id,
                                uint64_t value) {
    for (const SummaryScalarFieldMap &map : SUMMARY_SCALAR_FIELD_MAP) {
        if (map.field == field_id) {
            (void)set_summary_field_value(record, map.summary, value);
            return;
        }
    }
}

void apply_summary_metric_field(ReportSummaryRecord &record,
                                uint32_t field_id,
                                uint32_t subfield_id,
                                uint64_t value) {
    for (const SummaryMetricFieldMap &map : SUMMARY_METRIC_FIELD_MAP) {
        if (map.field == field_id && map.subfield == subfield_id) {
            (void)set_summary_field_value(record, map.summary, value);
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

        apply_summary_metric_field(record,
                                   field_id,
                                   field.field,
                                   field.value);
    }
}

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

        apply_summary_scalar_field(out, field.field, field.value);
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
    if (!out.end_ms) {
        out.end_ms = out.start_ms + 24ULL * 60ULL * 60ULL * 1000ULL;
    }
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

bool report_summary_field_value(const ReportSummaryRecord &record,
                                ReportSummaryField field,
                                uint32_t &out) {
    const size_t index = static_cast<size_t>(field);
    if (index >= AC_REPORT_SUMMARY_FIELD_COUNT || index >= 64) return false;
    if ((record.summary_field_mask & (1ULL << index)) == 0) return false;

    out = record.summary_field_values[index];
    return true;
}

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
