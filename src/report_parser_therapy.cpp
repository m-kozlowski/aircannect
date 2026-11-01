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
        {4, ReportSignalId::RespiratoryRate, 1.0 / 4.0, 8},
        {5, ReportSignalId::InspiratoryDuration, 1.0 / 25.0, 4},
        {6, ReportSignalId::MinuteVentilation, 2.0 / 5.0, 4},
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

bool build_therapy_1minute_chunk(const TherapyOneMinuteSignalSpec &spec,
                                 uint64_t interval_ms,
                                 const uint8_t *field_data,
                                 size_t field_len,
                                 ReportParsedChunk &chunk,
                                 ReportSpoolBuffer &output,
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

    if (!report_build_series_payload_v2_uniform_values_le(
            output,
            static_cast<uint32_t>(interval_ms),
            values.data(),
            sample_count)) {
        report_series_set_error(error,
                                error_len,
                                "therapy_series_v2_build_failed");
        return false;
    }

    chunk.source = ReportSourceId::TherapyOneMinute;
    chunk.kind = ReportParsedChunkKind::Series;
    chunk.name = report_signal_store_name(spec.signal);
    chunk.start_ms = static_cast<int64_t>(start);
    chunk.end_ms =
        chunk.start_ms + static_cast<int64_t>(sample_count * interval_ms);
    chunk.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    chunk.record_count = sample_count;
    chunk.payload = output.data();
    chunk.payload_len = output.size();
    return true;
}

}  // namespace

ReportSeriesReadResult ReportSeriesSpoolReader::next_therapy(
    ReportParsedChunk &chunk,
    ReportSpoolBuffer &output,
    char *error,
    size_t error_len) {
    while (true) {
        if (!therapy_record_active_) {
            if (therapy_wrapped_) {
                if (cursor_ >= result_->payload.size()) {
                    return ReportSeriesReadResult::Done;
                }

                ReportProtoField field;
                if (!report_proto_next(result_->payload.data(),
                                       result_->payload.size(),
                                       cursor_,
                                       field)) {
                    report_series_set_error(error,
                                            error_len,
                                            "therapy_outer_decode_failed");
                    return ReportSeriesReadResult::Failed;
                }

                therapy_record_data_ = field.data;
                therapy_record_len_ = field.len;
            } else if (therapy_raw_pending_) {
                therapy_raw_pending_ = false;
                therapy_record_data_ = result_->payload.data();
                therapy_record_len_ = result_->payload.size();
            } else {
                return ReportSeriesReadResult::Done;
            }

            therapy_record_active_ = true;
            therapy_record_initialized_ = false;
        }

        if (!therapy_record_initialized_) {
            uint64_t interval_token = 0;
            size_t index = 0;
            while (index < therapy_record_len_) {
                ReportProtoField field;
                if (!report_proto_next(therapy_record_data_,
                                       therapy_record_len_,
                                       index,
                                       field)) {
                    report_series_set_error(error,
                                            error_len,
                                            "therapy_record_decode_failed");
                    return ReportSeriesReadResult::Failed;
                }

                if (field.field == 15 && field.wire == 0) {
                    interval_token = field.value;
                    break;
                }
            }

            therapy_interval_ms_ = therapy_1minute_interval_ms(
                interval_token, interval_token != 0);
            therapy_signal_cursor_ = 0;
            therapy_record_initialized_ = true;
        }

        while (therapy_signal_cursor_ < therapy_record_len_) {
            ReportProtoField field;
            if (!report_proto_next(therapy_record_data_,
                                   therapy_record_len_,
                                   therapy_signal_cursor_,
                                   field)) {
                report_series_set_error(error,
                                        error_len,
                                        "therapy_record_decode_failed");
                return ReportSeriesReadResult::Failed;
            }

            const TherapyOneMinuteSignalSpec *spec =
                field.wire == 2 ? therapy_1minute_spec(field.field) : nullptr;
            if (!spec) continue;

            if (!build_therapy_1minute_chunk(*spec,
                                             therapy_interval_ms_,
                                             field.data,
                                             field.len,
                                             chunk,
                                             output,
                                             error,
                                             error_len)) {
                return ReportSeriesReadResult::Failed;
            }

            return ReportSeriesReadResult::Chunk;
        }

        therapy_record_active_ = false;
        therapy_record_initialized_ = false;
        therapy_record_data_ = nullptr;
        therapy_record_len_ = 0;

        if (!therapy_wrapped_) return ReportSeriesReadResult::Done;
    }
}

}  // namespace aircannect
