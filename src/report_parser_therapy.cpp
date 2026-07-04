#include "report_parser_series_internal.h"

#include <limits.h>

#include "report_records.h"

namespace aircannect {
namespace {

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
                          int64_t raw,
                          double scale,
                          char *error,
                          size_t error_len) {
    if (!report_series_append_scaled_value_le(payload, raw, scale)) {
        report_series_set_error(error,
                                error_len,
                                "therapy_series_append_failed");
        return false;
    }

    return true;
}

bool parse_therapy_1minute_blob(const uint8_t *blob,
                                size_t blob_len,
                                uint64_t interval_ms,
                                const TherapyOneMinuteSignalSpec &spec,
                                ReportSpoolBuffer &payload,
                                uint32_t &sample_count,
                                char *error,
                                size_t error_len) {
    sample_count = 0;
    payload.clear();

    if (!blob || blob_len < 2 || interval_ms == 0) {
        report_series_set_error(error, error_len, "bad_therapy_series_blob");
        return false;
    }

    if (interval_ms > UINT32_MAX) {
        report_series_set_error(error, error_len, "therapy_interval_invalid");
        return false;
    }

    const size_t max_samples =
        (blob_len <= 4 || spec.rice_m <= 0)
            ? blob_len / 2
            : (blob_len - 4) * 4u + 2u;
    if (max_samples == 0 || max_samples > UINT32_MAX ||
        max_samples > SIZE_MAX / 4u ||
        !payload.reserve_capacity(max_samples * 4u)) {
        report_series_set_error(error,
                                error_len,
                                "therapy_series_alloc_failed");
        return false;
    }

    if (blob_len <= 4 || spec.rice_m <= 0) {
        for (size_t offset = 0; offset + 1 < blob_len; offset += 2) {
            const int64_t raw = report_series_read_le_i16(blob + offset);

            if (!append_therapy_value(payload,
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

    int64_t prev2 = report_series_read_le_i16(blob);
    int64_t prev1 = report_series_read_le_i16(blob + 2);
    if (!append_therapy_value(payload,
                              prev2,
                              spec.scale,
                              error,
                              error_len) ||
        !append_therapy_value(payload,
                              prev1,
                              spec.scale,
                              error,
                              error_len)) {
        return false;
    }

    sample_count = 2;

    ReportSeriesBitReader bits(blob + 4, blob_len - 4);
    while (true) {
        uint64_t encoded = 0;
        if (!report_series_read_rice(bits, spec.rice_m, encoded)) break;

        const int64_t delta2 = report_series_zigzag_decode(encoded);
        const int64_t raw = 2 * prev1 - prev2 + delta2;
        prev2 = prev1;
        prev1 = raw;

        if (!append_therapy_value(payload,
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
        start > INT64_MAX || interval_ms > INT64_MAX) {
        report_series_set_error(error,
                                error_len,
                                "therapy_signal_decode_failed");
        return false;
    }

    ReportSpoolBuffer values;
    uint32_t sample_count = 0;
    if (!parse_therapy_1minute_blob(blob,
                                    blob_len,
                                    interval_ms,
                                    spec,
                                    values,
                                    sample_count,
                                    error,
                                    error_len)) {
        return false;
    }

    if (sample_count == 0 || sample_count > INT64_MAX / interval_ms) {
        report_series_set_error(error,
                                error_len,
                                "therapy_signal_count_invalid");
        return false;
    }

    ReportSpoolBuffer parsed;
    if (!report_build_series_payload_v2_uniform_values_le(
            parsed,
            static_cast<uint32_t>(interval_ms),
            values.data(),
            sample_count)) {
        report_series_set_error(error,
                                error_len,
                                "therapy_series_v2_build_failed");
        return false;
    }

    ReportParsedChunk chunk;
    chunk.source = ReportSourceId::TherapyOneMinute;
    chunk.kind = ReportStoreChunkKind::Series;
    chunk.name = report_signal_store_name(spec.signal);
    chunk.start_ms = static_cast<int64_t>(start);
    chunk.end_ms =
        chunk.start_ms + static_cast<int64_t>(sample_count * interval_ms);
    chunk.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    chunk.record_count = sample_count;
    chunk.payload = parsed.data();
    chunk.payload_len = parsed.size();

    if (!callback(context, chunk)) {
        if (!error || !error[0]) {
            report_series_set_error(error,
                                    error_len,
                                    "therapy_series_chunk_rejected");
        }

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
            report_series_set_error(error,
                                    error_len,
                                    "therapy_record_decode_failed");
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
            report_series_set_error(error,
                                    error_len,
                                    "therapy_record_decode_failed");
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

}  // namespace

bool report_parse_therapy_1minute_spool(const ReportSpoolResult &result,
                                        ReportParsedChunkCallback callback,
                                        void *context,
                                        char *error,
                                        size_t error_len) {
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
                report_series_set_error(error,
                                        error_len,
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
    report_series_set_error(error, error_len, "");
    return true;
}

}  // namespace aircannect
