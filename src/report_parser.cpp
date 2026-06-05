#include "report_parser.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "report_records.h"

namespace aircannect {
namespace {

void set_error(char *error, size_t error_len, const char *message) {
    if (!error || error_len == 0) return;
    snprintf(error, error_len, "%s", message ? message : "");
}

bool strings_match(const std::string &left, const char *right) {
    return right && strcmp(left.c_str(), right) == 0;
}

bool is_event_source(ReportSourceId source) {
    return source == ReportSourceId::UsageEvents ||
           source == ReportSourceId::RespiratoryEvents;
}

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

struct TherapyOneMinuteSignalSpec {
    uint32_t field = 0;
    ReportSignalId signal = ReportSignalId::Flow;
    double scale = 1.0;
    int32_t rice_m = 0;
};

const TherapyOneMinuteSignalSpec *therapy_1minute_spec(uint32_t field) {
    static const TherapyOneMinuteSignalSpec specs[] = {
        {1, ReportSignalId::Leak, 60.0 / 50.0, 4},
        {2, ReportSignalId::InspiratoryPressure, 1.0 / 5.0, 4},
        {3, ReportSignalId::ExpiratoryPressure, 1.0 / 5.0, 2},
        {4, ReportSignalId::MinuteVentilation, 1.0 / 8.0, 8},
        {5, ReportSignalId::InspiratoryDuration, 1.0 / 50.0, 4},
        {6, ReportSignalId::RespiratoryRate, 1.0, 4},
        {7, ReportSignalId::IeRatio, 4.0, 4},
    };
    for (const TherapyOneMinuteSignalSpec &spec : specs) {
        if (spec.field == field) return &spec;
    }
    return nullptr;
}

int64_t zigzag_decode(uint64_t value) {
    return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
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
        const int64_t decoded = zigzag_decode(raw);
        if (decoded < INT32_MIN || decoded > INT32_MAX) return false;
        params[params_count++] = static_cast<int32_t>(decoded);
    }
    return true;
}

class Rc03BitReader {
public:
    Rc03BitReader(const uint8_t *data, size_t len)
        : data_(data), len_(len) {}

    bool read_bit(uint8_t &out) {
        if (!data_ || byte_index_ >= len_) return false;
        out = static_cast<uint8_t>(
            (data_[byte_index_] >> (7 - bit_index_)) & 1u);
        bit_index_++;
        if (bit_index_ == 8) {
            bit_index_ = 0;
            byte_index_++;
        }
        return true;
    }

private:
    const uint8_t *data_ = nullptr;
    size_t len_ = 0;
    size_t byte_index_ = 0;
    uint8_t bit_index_ = 0;
};

bool power_of_two_positive(int32_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

bool rc03_read_rice(Rc03BitReader &bits, int32_t modulus, uint64_t &out) {
    if (!power_of_two_positive(modulus)) return false;

    uint64_t q = 0;
    uint8_t bit = 0;
    while (true) {
        if (!bits.read_bit(bit)) return false;
        if (bit == 0) break;
        if (q == UINT64_MAX) return false;
        q++;
    }

    uint64_t rem = 0;
    int32_t width = 0;
    for (int32_t m = modulus; m > 1; m >>= 1) width++;
    for (int32_t i = 0; i < width; ++i) {
        if (!bits.read_bit(bit)) return false;
        rem = (rem << 1) | bit;
    }

    if (q > (UINT64_MAX - rem) / static_cast<uint64_t>(modulus)) {
        return false;
    }
    out = q * static_cast<uint64_t>(modulus) + rem;
    return true;
}

int16_t read_le_i16(const uint8_t *data) {
    const uint16_t value = static_cast<uint16_t>(data[0]) |
                           (static_cast<uint16_t>(data[1]) << 8);
    return static_cast<int16_t>(value);
}

int32_t milli_from_scaled(int64_t raw, double scale) {
    const double value = static_cast<double>(raw) * scale * 1000.0;
    if (value >= 0.0) {
        return static_cast<int32_t>(value + 0.5);
    }
    return static_cast<int32_t>(value - 0.5);
}

bool parse_rc03_block(const uint8_t *block,
                      size_t block_len,
                      int64_t start_ms,
                      uint64_t interval_ms,
                      uint64_t sample_count,
                      ReportSpoolBuffer &payload,
                      char *error,
                      size_t error_len) {
    if (!block || block_len < 6 || sample_count == 0 ||
        sample_count > SIZE_MAX / report_series_sample_wire_size()) {
        set_error(error, error_len, "bad_rc03_block");
        return false;
    }

    const uint8_t header_len = block[0];
    if (header_len < 4 || block_len < 1 + header_len) {
        set_error(error, error_len, "bad_rc03_header");
        return false;
    }
    const uint8_t *header = block + 1;
    if (memcmp(header, "RC03", 4) != 0) {
        set_error(error, error_len, "missing_rc03_magic");
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
        set_error(error, error_len, "bad_rc03_params");
        return false;
    }

    const int32_t rice_m = params[4];
    if (!power_of_two_positive(rice_m)) {
        set_error(error, error_len, "bad_rc03_rice");
        return false;
    }

    const double scale = 2.0 * pow(10.0, static_cast<double>(params[1]));
    const uint8_t *body = block + 1 + header_len;
    const size_t body_len = block_len - 1 - header_len;
    if ((sample_count >= 1 && body_len < 2) ||
        (sample_count >= 2 && body_len < 4)) {
        set_error(error, error_len, "rc03_seed_missing");
        return false;
    }

    payload.clear();
    if (!payload.reserve_capacity(
            static_cast<size_t>(sample_count) *
            report_series_sample_wire_size())) {
        set_error(error, error_len, "series_alloc_failed");
        return false;
    }

    int64_t prev2 = 0;
    int64_t prev1 = 0;
    size_t body_offset = 0;
    for (uint64_t i = 0; i < sample_count; ++i) {
        int64_t raw = 0;
        if (i == 0) {
            raw = read_le_i16(body);
            body_offset = 2;
        } else if (i == 1) {
            raw = read_le_i16(body + 2);
            body_offset = 4;
        } else {
            break;
        }

        ReportSeriesSample sample;
        sample.timestamp_ms =
            start_ms + static_cast<int64_t>(i * interval_ms);
        sample.value_milli = milli_from_scaled(raw, scale);
        if (!report_append_series_sample(payload, sample)) {
            set_error(error, error_len, "series_append_failed");
            return false;
        }
        if (i == 0) prev2 = raw;
        else prev1 = raw;
    }

    if (sample_count <= 2) return true;

    Rc03BitReader bits(body + body_offset, body_len - body_offset);
    for (uint64_t i = 2; i < sample_count; ++i) {
        uint64_t encoded = 0;
        if (!rc03_read_rice(bits, rice_m, encoded)) {
            set_error(error, error_len, "rc03_bitstream_short");
            return false;
        }
        const int64_t delta2 = zigzag_decode(encoded);
        const int64_t raw = 2 * prev1 - prev2 + delta2;
        prev2 = prev1;
        prev1 = raw;

        ReportSeriesSample sample;
        sample.timestamp_ms =
            start_ms + static_cast<int64_t>(i * interval_ms);
        sample.value_milli = milli_from_scaled(raw, scale);
        if (!report_append_series_sample(payload, sample)) {
            set_error(error, error_len, "series_append_failed");
            return false;
        }
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

uint64_t therapy_1minute_interval_ms(uint64_t token, bool present) {
    if (!present) return 60000;
    if (token < 1000) return token * 60000;
    return token;
}

bool parse_therapy_signal_fields(const uint8_t *data,
                                 size_t len,
                                 uint64_t &start_ms,
                                 const uint8_t *&blob,
                                 size_t &blob_len) {
    start_ms = 0;
    blob = nullptr;
    blob_len = 0;

    size_t index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) return false;
        if (field.field == 2 && field.wire == 0) {
            start_ms = field.value;
        } else if (field.field == 3 && field.wire == 2) {
            blob = field.data;
            blob_len = field.len;
        }
    }
    return start_ms > 0 && blob && blob_len;
}

bool append_therapy_value(ReportSpoolBuffer &payload,
                          int64_t start_ms,
                          uint64_t interval_ms,
                          uint64_t sample_index,
                          int64_t raw,
                          double scale,
                          char *error,
                          size_t error_len) {
    ReportSeriesSample sample;
    sample.timestamp_ms =
        start_ms + static_cast<int64_t>(sample_index * interval_ms);
    sample.value_milli = milli_from_scaled(raw, scale);
    if (!report_append_series_sample(payload, sample)) {
        set_error(error, error_len, "therapy_series_append_failed");
        return false;
    }
    return true;
}

bool parse_therapy_1minute_blob(const uint8_t *blob,
                                size_t blob_len,
                                int64_t start_ms,
                                uint64_t interval_ms,
                                const TherapyOneMinuteSignalSpec &spec,
                                ReportSpoolBuffer &payload,
                                uint32_t &sample_count,
                                char *error,
                                size_t error_len) {
    sample_count = 0;
    payload.clear();
    if (!blob || blob_len < 2 || interval_ms == 0) {
        set_error(error, error_len, "bad_therapy_series_blob");
        return false;
    }
    if (!payload.reserve_capacity(
            (blob_len / 2 + blob_len) * report_series_sample_wire_size())) {
        set_error(error, error_len, "therapy_series_alloc_failed");
        return false;
    }

    if (blob_len <= 4 || spec.rice_m <= 0) {
        for (size_t offset = 0; offset + 1 < blob_len; offset += 2) {
            const int64_t raw = read_le_i16(blob + offset);
            if (!append_therapy_value(payload,
                                      start_ms,
                                      interval_ms,
                                      sample_count,
                                      raw,
                                      spec.scale,
                                      error,
                                      error_len)) {
                return false;
            }
            sample_count++;
        }
        return sample_count > 0;
    }

    int64_t prev2 = read_le_i16(blob);
    int64_t prev1 = read_le_i16(blob + 2);
    if (!append_therapy_value(payload,
                              start_ms,
                              interval_ms,
                              0,
                              prev2,
                              spec.scale,
                              error,
                              error_len) ||
        !append_therapy_value(payload,
                              start_ms,
                              interval_ms,
                              1,
                              prev1,
                              spec.scale,
                              error,
                              error_len)) {
        return false;
    }
    sample_count = 2;

    Rc03BitReader bits(blob + 4, blob_len - 4);
    while (true) {
        uint64_t encoded = 0;
        if (!rc03_read_rice(bits, spec.rice_m, encoded)) break;
        const int64_t delta2 = zigzag_decode(encoded);
        const int64_t raw = 2 * prev1 - prev2 + delta2;
        prev2 = prev1;
        prev1 = raw;
        if (!append_therapy_value(payload,
                                  start_ms,
                                  interval_ms,
                                  sample_count,
                                  raw,
                                  spec.scale,
                                  error,
                                  error_len)) {
            return false;
        }
        sample_count++;
    }
    return sample_count > 0;
}

bool emit_therapy_1minute_signal(const TherapyOneMinuteSignalSpec &spec,
                                 uint64_t interval_ms,
                                 const uint8_t *field_data,
                                 size_t field_len,
                                 ReportParsedChunkCallback callback,
                                 void *context,
                                 char *error,
                                 size_t error_len) {
    uint64_t start = 0;
    const uint8_t *blob = nullptr;
    size_t blob_len = 0;
    if (!parse_therapy_signal_fields(field_data,
                                     field_len,
                                     start,
                                     blob,
                                     blob_len) ||
        start > INT64_MAX ||
        interval_ms > INT64_MAX) {
        set_error(error, error_len, "therapy_signal_decode_failed");
        return false;
    }

    ReportSpoolBuffer parsed;
    uint32_t sample_count = 0;
    if (!parse_therapy_1minute_blob(blob,
                                    blob_len,
                                    static_cast<int64_t>(start),
                                    interval_ms,
                                    spec,
                                    parsed,
                                    sample_count,
                                    error,
                                    error_len)) {
        return false;
    }
    if (sample_count == 0 ||
        sample_count > INT64_MAX / interval_ms) {
        set_error(error, error_len, "therapy_signal_count_invalid");
        return false;
    }

    ReportParsedChunk chunk;
    chunk.source = ReportSourceId::TherapyOneMinute;
    chunk.kind = ReportStoreChunkKind::Series;
    chunk.name = report_signal_store_name(spec.signal);
    chunk.start_ms = static_cast<int64_t>(start);
    chunk.end_ms =
        chunk.start_ms + static_cast<int64_t>(sample_count * interval_ms);
    chunk.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V1;
    chunk.record_count = sample_count;
    chunk.payload = parsed.data();
    chunk.payload_len = parsed.size();
    if (!callback(context, chunk)) {
        set_error(error, error_len, "therapy_series_chunk_rejected");
        return false;
    }
    return true;
}

bool parse_therapy_1minute_record(const uint8_t *data,
                                  size_t len,
                                  ReportParsedChunkCallback callback,
                                  void *context,
                                  uint32_t &chunk_count,
                                  char *error,
                                  size_t error_len) {
    uint64_t interval_token = 0;
    size_t index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) {
            set_error(error, error_len, "therapy_record_decode_failed");
            return false;
        }
        if (field.field == 15 && field.wire == 0) {
            interval_token = field.value;
            break;
        }
    }
    const uint64_t interval_ms =
        therapy_1minute_interval_ms(interval_token, interval_token != 0);

    index = 0;
    while (index < len) {
        ReportProtoField field;
        if (!report_proto_next(data, len, index, field)) {
            set_error(error, error_len, "therapy_record_decode_failed");
            return false;
        }
        const TherapyOneMinuteSignalSpec *spec =
            field.wire == 2 ? therapy_1minute_spec(field.field) : nullptr;
        if (!spec) continue;
        if (!emit_therapy_1minute_signal(*spec,
                                         interval_ms,
                                         field.data,
                                         field.len,
                                         callback,
                                         context,
                                         error,
                                         error_len)) {
            return false;
        }
        chunk_count++;
    }
    return true;
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

bool report_validate_spool_for_source(const ReportSpoolResult &result,
                                      ReportSourceId source,
                                      char *error,
                                      size_t error_len) {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        set_error(error, error_len, "unknown_report_source");
        return false;
    }
    if (!strings_match(result.spool_type, def->spool_type)) {
        set_error(error, error_len, "wrong_report_source");
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

bool report_parse_summary_spool(const ReportSpoolResult &result,
                                ReportSummaryRecordCallback callback,
                                void *context,
                                char *error,
                                size_t error_len) {
    if (!report_validate_spool_for_source(result,
                                          ReportSourceId::Summary,
                                          error,
                                          error_len)) {
        return false;
    }
    return report_parse_summary_records(result.payload.data(),
                                        result.payload.size(),
                                        callback,
                                        context,
                                        error,
                                        error_len);
}

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
    chunk.kind = ReportStoreChunkKind::Events;
    chunk.name = def ? def->spool_type : "";
    chunk.start_ms = parsed.min_start_ms;
    chunk.end_ms = parsed.max_end_ms;
    chunk.payload_schema = REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1;
    chunk.record_count = parsed.record_count;
    chunk.payload = parsed.payload.data();
    chunk.payload_len = parsed.payload.size();
    if (!callback(context, chunk)) {
        set_error(error, error_len, "event_chunk_rejected");
        return false;
    }
    set_error(error, error_len, "");
    return true;
}

bool report_parse_series_spool(const ReportSpoolResult &result,
                               ReportSourceId source,
                               ReportParsedChunkCallback callback,
                               void *context,
                               char *error,
                               size_t error_len) {
    if (!is_series_source(source)) {
        set_error(error, error_len, "unsupported_series_source");
        return false;
    }
    if (!callback) {
        set_error(error, error_len, "missing_chunk_callback");
        return false;
    }
    if (!report_validate_spool_for_source(result, source, error, error_len)) {
        return false;
    }

    if (source == ReportSourceId::TherapyOneMinute) {
        uint32_t chunk_count = 0;
        if (report_proto_all_length_fields(result.payload.data(),
                                           result.payload.size(),
                                           5)) {
            size_t index = 0;
            while (index < result.payload.size()) {
                ReportProtoField field;
                if (!report_proto_next(result.payload.data(),
                                       result.payload.size(),
                                       index,
                                       field)) {
                    set_error(error, error_len,
                              "therapy_outer_decode_failed");
                    return false;
                }
                if (!parse_therapy_1minute_record(field.data,
                                                  field.len,
                                                  callback,
                                                  context,
                                                  chunk_count,
                                                  error,
                                                  error_len)) {
                    return false;
                }
            }
        } else if (!parse_therapy_1minute_record(result.payload.data(),
                                                 result.payload.size(),
                                                 callback,
                                                 context,
                                                 chunk_count,
                                                 error,
                                                 error_len)) {
            return false;
        }
        (void)chunk_count;
        set_error(error, error_len, "");
        return true;
    }

    const uint32_t expected_field = rc03_wire_field(source);
    const ReportSourceDef *source_def = report_source_def(source);
    const ReportSignalId signal_id = rc03_signal(source);
    const char *signal_name = report_signal_store_name(signal_id);
    if (!expected_field || !source_def || !signal_name || !signal_name[0]) {
        set_error(error, error_len, "bad_series_mapping");
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
            set_error(error, error_len, "series_outer_decode_failed");
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
            set_error(error, error_len, "series_record_decode_failed");
            return false;
        }
        if (start_ms > INT64_MAX || end_ms > INT64_MAX ||
            interval_ms > INT64_MAX || end_ms < start_ms) {
            set_error(error, error_len, "series_time_invalid");
            return false;
        }

        const uint64_t sample_count =
            ((end_ms - start_ms) / interval_ms) + 1;
        if (sample_count == 0 || sample_count > UINT32_MAX ||
            sample_count > INT64_MAX / interval_ms) {
            set_error(error, error_len, "series_count_invalid");
            return false;
        }
        const int64_t chunk_start = static_cast<int64_t>(start_ms);
        const int64_t chunk_end =
            chunk_start + static_cast<int64_t>(sample_count * interval_ms);
        if (chunk_end <= chunk_start) {
            set_error(error, error_len, "series_range_invalid");
            return false;
        }

        ReportSpoolBuffer parsed;
        if (!parse_rc03_block(block,
                              block_len,
                              chunk_start,
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
        chunk.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V1;
        chunk.record_count = static_cast<uint32_t>(sample_count);
        chunk.payload = parsed.data();
        chunk.payload_len = parsed.size();
        if (!callback(context, chunk)) {
            set_error(error, error_len, "series_chunk_rejected");
            return false;
        }
        emitted++;
    }

    (void)emitted;
    set_error(error, error_len, "");
    return true;
}

}  // namespace aircannect
