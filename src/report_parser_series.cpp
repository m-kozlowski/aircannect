#include "report_parser_series_internal.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "report_records.h"

namespace aircannect {
namespace {

bool is_rc03_source(ReportSourceId source) {
    return source == ReportSourceId::RespiratoryFlow6p25Hz ||
           source == ReportSourceId::MaskPressure6p25Hz ||
           source == ReportSourceId::InspiratoryPressure0p5Hz ||
           source == ReportSourceId::Leak0p5Hz;
}

bool is_series_source(ReportSourceId source) {
    return is_rc03_source(source) ||
           source == ReportSourceId::TherapyOneMinute;
}

uint32_t rc03_wire_field(ReportSourceId source) {
    switch (source) {
        case ReportSourceId::RespiratoryFlow6p25Hz: return 18;
        case ReportSourceId::MaskPressure6p25Hz: return 19;
        case ReportSourceId::Leak0p5Hz: return 20;
        case ReportSourceId::InspiratoryPressure0p5Hz: return 21;
        default: return 0;
    }
}

ReportSignalId rc03_signal(ReportSourceId source) {
    switch (source) {
        case ReportSourceId::RespiratoryFlow6p25Hz:
            return ReportSignalId::Flow;
        case ReportSourceId::MaskPressure6p25Hz:
            return ReportSignalId::MaskPressure;
        case ReportSourceId::Leak0p5Hz:
            return ReportSignalId::Leak;
        case ReportSourceId::InspiratoryPressure0p5Hz:
            return ReportSignalId::InspiratoryPressure;
        default:
            return ReportSignalId::Flow;
    }
}

bool decode_rc03_params(const uint8_t *data,
                        size_t len,
                        int32_t *params,
                        size_t params_max,
                        size_t &params_count) {
    params_count = 0;
    size_t index = 0;
    while (index < len) {
        if (params_count >= params_max) return false;

        uint64_t raw = 0;
        if (!report_proto_read_varint(data, len, index, raw)) return false;

        const int64_t decoded = report_series_zigzag_decode(raw);
        if (decoded < INT32_MIN || decoded > INT32_MAX) return false;

        params[params_count++] = static_cast<int32_t>(decoded);
    }
    return true;
}

bool parse_rc03_block(const uint8_t *block,
                      size_t block_len,
                      uint64_t interval_ms,
                      uint64_t sample_count,
                      ReportSpoolBuffer &payload,
                      char *error,
                      size_t error_len) {
    if (!block || block_len < 6 || sample_count == 0 ||
        sample_count > UINT32_MAX || sample_count > SIZE_MAX / 4u ||
        interval_ms == 0 || interval_ms > UINT32_MAX) {
        report_series_set_error(error, error_len, "bad_rc03_block");
        return false;
    }

    const uint8_t header_len = block[0];
    if (header_len < 4 || block_len < 1 + header_len) {
        report_series_set_error(error, error_len, "bad_rc03_header");
        return false;
    }

    const uint8_t *header = block + 1;
    if (memcmp(header, "RC03", 4) != 0) {
        report_series_set_error(error, error_len, "missing_rc03_magic");
        return false;
    }

    int32_t params[12];
    size_t params_count = 0;
    if (!decode_rc03_params(header + 4,
                            header_len - 4,
                            params,
                            sizeof(params) / sizeof(params[0]),
                            params_count) ||
        params_count < 5) {
        report_series_set_error(error, error_len, "bad_rc03_params");
        return false;
    }

    const int32_t rice_m = params[4];
    if (!report_series_power_of_two_positive(rice_m)) {
        report_series_set_error(error, error_len, "bad_rc03_rice");
        return false;
    }

    const double scale = 2.0 * pow(10.0, static_cast<double>(params[1]));
    const uint8_t *body = block + 1 + header_len;
    const size_t body_len = block_len - 1 - header_len;
    if ((sample_count >= 1 && body_len < 2) ||
        (sample_count >= 2 && body_len < 4)) {
        report_series_set_error(error, error_len, "rc03_seed_missing");
        return false;
    }

    ReportSpoolBuffer values;
    if (!values.reserve_capacity(static_cast<size_t>(sample_count) * 4u)) {
        report_series_set_error(error, error_len, "series_alloc_failed");
        return false;
    }

    int64_t prev2 = 0;
    int64_t prev1 = 0;
    size_t body_offset = 0;
    for (uint64_t i = 0; i < sample_count; ++i) {
        int64_t raw = 0;
        if (i == 0) {
            raw = report_series_read_le_i16(body);
            body_offset = 2;
        } else if (i == 1) {
            raw = report_series_read_le_i16(body + 2);
            body_offset = 4;
        } else {
            break;
        }

        if (!report_series_append_scaled_value_le(values, raw, scale)) {
            report_series_set_error(error, error_len, "series_append_failed");
            return false;
        }
        if (i == 0) prev2 = raw;
        else prev1 = raw;
    }

    if (sample_count > 2) {
        ReportSeriesBitReader bits(body + body_offset,
                                   body_len - body_offset);
        for (uint64_t i = 2; i < sample_count; ++i) {
            uint64_t encoded = 0;
            if (!report_series_read_rice(bits, rice_m, encoded)) {
                report_series_set_error(error,
                                        error_len,
                                        "rc03_bitstream_short");
                return false;
            }

            const int64_t delta2 = report_series_zigzag_decode(encoded);
            const int64_t raw = 2 * prev1 - prev2 + delta2;
            prev2 = prev1;
            prev1 = raw;

            if (!report_series_append_scaled_value_le(values, raw, scale)) {
                report_series_set_error(error,
                                        error_len,
                                        "series_append_failed");
                return false;
            }
        }
    }

    if (values.size() != static_cast<size_t>(sample_count) * 4u ||
        !report_build_series_payload_v2_uniform_values_le(
            payload,
            static_cast<uint32_t>(interval_ms),
            values.data(),
            static_cast<uint32_t>(sample_count))) {
        report_series_set_error(error, error_len, "series_v2_build_failed");
        return false;
    }
    return true;
}

bool parse_rc03_payload_fields(const uint8_t *data,
                               size_t len,
                               uint64_t &interval_ms,
                               uint64_t &start_ms,
                               uint64_t &end_ms,
                               const uint8_t *&block,
                               size_t &block_len) {
    interval_ms = 0;
    start_ms = 0;
    end_ms = 0;
    block = nullptr;
    block_len = 0;

    size_t index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;

        if (field.field == 1 && field.wire == 0) {
            interval_ms = field.value;
        } else if (field.field == 2 && field.wire == 0) {
            start_ms = field.value;
        } else if (field.field == 3 && field.wire == 0) {
            end_ms = field.value;
        } else if (field.field == 4 && field.wire == 2) {
            block = field.data;
            block_len = field.len;
        }
    }
    return interval_ms > 0 && start_ms > 0 && end_ms >= start_ms &&
           block && block_len;
}

bool parse_rc03_record(const uint8_t *data,
                       size_t len,
                       uint64_t &interval_ms,
                       uint64_t &start_ms,
                       uint64_t &end_ms,
                       const uint8_t *&block,
                       size_t &block_len) {
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;

    size_t index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;
        if (field.field == 2 && field.wire == 2) {
            payload = field.data;
            payload_len = field.len;
        }
    }
    return payload &&
           parse_rc03_payload_fields(payload,
                                     payload_len,
                                     interval_ms,
                                     start_ms,
                                     end_ms,
                                     block,
                                     block_len);
}

}  // namespace

bool report_parse_series_spool(const ReportSpoolResult &result,
                               ReportSourceId source,
                               ReportParsedChunkCallback callback,
                               void *context,
                               char *error,
                               size_t error_len) {
    if (!is_series_source(source)) {
        report_series_set_error(error, error_len, "unsupported_series_source");
        return false;
    }
    if (!callback) {
        report_series_set_error(error, error_len, "missing_chunk_callback");
        return false;
    }
    if (!report_validate_spool_for_source(result, source, error, error_len)) {
        return false;
    }

    if (source == ReportSourceId::TherapyOneMinute) {
        return report_parse_therapy_1minute_spool(result,
                                                  callback,
                                                  context,
                                                  error,
                                                  error_len);
    }

    const uint32_t expected_field = rc03_wire_field(source);
    const ReportSourceDef *source_def = report_source_def(source);
    const ReportSignalId signal_id = rc03_signal(source);
    const char *signal_name = report_signal_store_name(signal_id);
    if (!expected_field || !source_def || !signal_name || !signal_name[0]) {
        report_series_set_error(error, error_len, "bad_series_mapping");
        return false;
    }

    uint32_t emitted = 0;
    size_t index = 0;
    while (index < result.payload.size()) {
        ReportProtoField field;
        if (!report_proto_next(result.payload.data(),
                               result.payload.size(),
                               index,
                               field)) {
            report_series_set_error(error,
                                    error_len,
                                    "series_outer_decode_failed");
            return false;
        }
        if (field.field != expected_field || field.wire != 2) continue;

        uint64_t interval_ms = 0;
        uint64_t start_ms = 0;
        uint64_t end_ms = 0;
        const uint8_t *block = nullptr;
        size_t block_len = 0;
        if (!parse_rc03_record(field.data,
                               field.len,
                               interval_ms,
                               start_ms,
                               end_ms,
                               block,
                               block_len)) {
            report_series_set_error(error,
                                    error_len,
                                    "series_record_decode_failed");
            return false;
        }
        if (start_ms > INT64_MAX || end_ms > INT64_MAX ||
            interval_ms > INT64_MAX || end_ms < start_ms) {
            report_series_set_error(error, error_len, "series_time_invalid");
            return false;
        }

        const uint64_t sample_count =
            ((end_ms - start_ms) / interval_ms) + 1;
        if (sample_count == 0 || sample_count > UINT32_MAX ||
            sample_count > INT64_MAX / interval_ms) {
            report_series_set_error(error, error_len, "series_count_invalid");
            return false;
        }

        const int64_t chunk_start = static_cast<int64_t>(start_ms);
        const int64_t chunk_end =
            chunk_start + static_cast<int64_t>(sample_count * interval_ms);
        if (chunk_end <= chunk_start) {
            report_series_set_error(error, error_len, "series_range_invalid");
            return false;
        }

        ReportSpoolBuffer parsed;
        if (!parse_rc03_block(block,
                              block_len,
                              interval_ms,
                              sample_count,
                              parsed,
                              error,
                              error_len)) {
            return false;
        }

        ReportParsedChunk chunk;
        chunk.source = source;
        chunk.kind = ReportStoreChunkKind::Series;
        chunk.name = signal_name;
        chunk.start_ms = chunk_start;
        chunk.end_ms = chunk_end;
        chunk.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
        chunk.record_count = static_cast<uint32_t>(sample_count);
        chunk.payload = parsed.data();
        chunk.payload_len = parsed.size();
        if (!callback(context, chunk)) {
            if (!error || !error[0]) {
                report_series_set_error(error,
                                        error_len,
                                        "series_chunk_rejected");
            }
            return false;
        }
        emitted++;
    }

    (void)emitted;
    report_series_set_error(error, error_len, "");
    return true;
}

}  // namespace aircannect
