#include "report_parser_series_internal.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "report_records.h"
#include "report_records_internal.h"

namespace aircannect {
namespace {

static constexpr uint32_t REPORT_SERIES_READ_WINDOW_SAMPLES = 4096;

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

bool prepare_rc03_block(const uint8_t *block,
                        size_t block_len,
                        uint64_t interval_ms,
                        uint64_t sample_count,
                        const uint8_t *&body,
                        size_t &body_len,
                        size_t &bit_body_offset,
                        int32_t &rice_m,
                        double &scale,
                        char *error,
                        size_t error_len) {
    body = nullptr;
    body_len = 0;
    bit_body_offset = 0;
    rice_m = 0;
    scale = 1.0;

    if (!block || block_len < 6 || sample_count == 0 ||
        sample_count > UINT32_MAX || sample_count > SIZE_MAX / 4u ||
        interval_ms == 0 || interval_ms > UINT32_MAX) {
        report_series_set_error(error, error_len, "bad_rc03_block");
        return false;
    }

    const uint8_t header_len = block[0];
    if (header_len < 4 ||
        block_len < 1u + static_cast<size_t>(header_len)) {
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

    rice_m = params[4];
    if (!report_series_power_of_two_positive(rice_m)) {
        report_series_set_error(error, error_len, "bad_rc03_rice");
        return false;
    }

    scale = 2.0 * pow(10.0, static_cast<double>(params[1]));
    body = block + 1 + header_len;
    body_len = block_len - 1 - header_len;
    if ((sample_count >= 1 && body_len < 2) ||
        (sample_count >= 2 && body_len < 4)) {
        report_series_set_error(error, error_len, "rc03_seed_missing");
        return false;
    }

    bit_body_offset = sample_count > 1 ? 4 : 2;
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

bool ReportSeriesSpoolReader::begin(const ReportSpoolResult &result,
                                    ReportSourceId source,
                                    char *error,
                                    size_t error_len) {
    reset();

    if (!is_series_source(source)) {
        report_series_set_error(error, error_len, "unsupported_series_source");
        return false;
    }
    if (!report_validate_spool_for_source(result, source, error, error_len)) {
        return false;
    }

    if (source == ReportSourceId::TherapyOneMinute) {
        result_ = &result;
        source_ = source;
        therapy_wrapped_ = report_proto_all_length_fields(
            result.payload.data(), result.payload.size(), 5);
        therapy_raw_pending_ = !therapy_wrapped_;
        ready_ = true;
        report_series_set_error(error, error_len, "");
        return true;
    }

    const uint32_t expected_field = rc03_wire_field(source);
    const ReportSourceDef *source_def = report_source_def(source);
    const ReportSignalId signal_id = rc03_signal(source);
    const char *signal_name = report_signal_store_name(signal_id);
    if (!expected_field || !source_def || !signal_name || !signal_name[0]) {
        report_series_set_error(error, error_len, "bad_series_mapping");
        return false;
    }

    result_ = &result;
    source_ = source;
    expected_field_ = expected_field;
    signal_name_ = signal_name;
    ready_ = true;
    report_series_set_error(error, error_len, "");
    return true;
}

void ReportSeriesSpoolReader::reset() {
    result_ = nullptr;
    source_ = ReportSourceId::Summary;
    cursor_ = 0;
    expected_field_ = 0;
    signal_name_ = nullptr;
    ready_ = false;
    failed_ = false;
    done_ = false;
    rc03_record_active_ = false;
    rc03_body_ = nullptr;
    rc03_body_len_ = 0;
    rc03_bit_body_offset_ = 0;
    rc03_bit_byte_index_ = 0;
    rc03_bit_index_ = 0;
    rc03_interval_ms_ = 0;
    rc03_start_ms_ = 0;
    rc03_sample_count_ = 0;
    rc03_samples_decoded_ = 0;
    rc03_rice_m_ = 0;
    rc03_scale_ = 1.0;
    rc03_prev2_ = 0;
    rc03_prev1_ = 0;
    therapy_wrapped_ = false;
    therapy_record_active_ = false;
    therapy_record_initialized_ = false;
    therapy_raw_pending_ = false;
    therapy_record_data_ = nullptr;
    therapy_record_len_ = 0;
    therapy_signal_cursor_ = 0;
    therapy_interval_ms_ = 60000;
}

ReportSeriesReadResult ReportSeriesSpoolReader::next(
    ReportParsedChunk &chunk,
    ReportSpoolBuffer &output,
    char *error,
    size_t error_len) {
    chunk = {};
    output.clear();

    if (failed_) {
        report_series_set_error(error, error_len, "series_reader_failed");
        return ReportSeriesReadResult::Failed;
    }
    if (!ready_ || !result_) {
        failed_ = true;
        report_series_set_error(error, error_len, "series_reader_not_ready");
        return ReportSeriesReadResult::Failed;
    }
    if (done_) {
        report_series_set_error(error, error_len, "");
        return ReportSeriesReadResult::Done;
    }

    const ReportSeriesReadResult read_result =
        source_ == ReportSourceId::TherapyOneMinute
            ? next_therapy(chunk, output, error, error_len)
            : next_rc03(chunk, output, error, error_len);
    if (read_result == ReportSeriesReadResult::Failed) {
        failed_ = true;
    } else if (read_result == ReportSeriesReadResult::Done) {
        done_ = true;
        report_series_set_error(error, error_len, "");
    }
    return read_result;
}

ReportSeriesReadResult ReportSeriesSpoolReader::next_rc03(
    ReportParsedChunk &chunk,
    ReportSpoolBuffer &output,
    char *error,
    size_t error_len) {
    if (!rc03_record_active_) {
        bool found_record = false;
        while (cursor_ < result_->payload.size()) {
            ReportProtoField field;
            if (!report_proto_next(result_->payload.data(),
                                   result_->payload.size(),
                                   cursor_,
                                   field)) {
                report_series_set_error(error,
                                        error_len,
                                        "series_outer_decode_failed");
                return ReportSeriesReadResult::Failed;
            }
            if (field.field != expected_field_ || field.wire != 2) continue;

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
                return ReportSeriesReadResult::Failed;
            }
            if (start_ms > INT64_MAX || end_ms > INT64_MAX ||
                interval_ms > INT64_MAX || end_ms < start_ms) {
                report_series_set_error(error,
                                        error_len,
                                        "series_time_invalid");
                return ReportSeriesReadResult::Failed;
            }

            const uint64_t sample_count =
                ((end_ms - start_ms) / interval_ms) + 1;
            if (sample_count == 0 || sample_count > UINT32_MAX ||
                sample_count > INT64_MAX / interval_ms) {
                report_series_set_error(error,
                                        error_len,
                                        "series_count_invalid");
                return ReportSeriesReadResult::Failed;
            }

            const uint64_t duration_ms = sample_count * interval_ms;
            if (duration_ms > static_cast<uint64_t>(INT64_MAX) - start_ms) {
                report_series_set_error(error,
                                        error_len,
                                        "series_range_invalid");
                return ReportSeriesReadResult::Failed;
            }

            if (!prepare_rc03_block(block,
                                    block_len,
                                    interval_ms,
                                    sample_count,
                                    rc03_body_,
                                    rc03_body_len_,
                                    rc03_bit_body_offset_,
                                    rc03_rice_m_,
                                    rc03_scale_,
                                    error,
                                    error_len)) {
                return ReportSeriesReadResult::Failed;
            }

            rc03_record_active_ = true;
            rc03_bit_byte_index_ = 0;
            rc03_bit_index_ = 0;
            rc03_interval_ms_ = interval_ms;
            rc03_start_ms_ = start_ms;
            rc03_sample_count_ = sample_count;
            rc03_samples_decoded_ = 0;
            rc03_prev2_ = 0;
            rc03_prev1_ = 0;
            found_record = true;
            break;
        }

        if (!found_record) return ReportSeriesReadResult::Done;
    }

    const uint64_t first_sample = rc03_samples_decoded_;
    const uint64_t remaining = rc03_sample_count_ - first_sample;
    const uint32_t window_count = static_cast<uint32_t>(
        remaining > REPORT_SERIES_READ_WINDOW_SAMPLES
            ? REPORT_SERIES_READ_WINDOW_SAMPLES
            : remaining);
    const size_t payload_size =
        report_series_v2_uniform_wire_size(window_count, 0);
    if (payload_size == 0 || !output.reserve_capacity(payload_size)) {
        report_series_set_error(error, error_len, "series_alloc_failed");
        return ReportSeriesReadResult::Failed;
    }
    if (!report_records_detail::put_series_v2_header(
            output, static_cast<uint32_t>(rc03_interval_ms_), window_count, 0)) {
        report_series_set_error(error, error_len, "series_v2_build_failed");
        return ReportSeriesReadResult::Failed;
    }

    ReportSeriesBitReader bits(rc03_body_ + rc03_bit_body_offset_,
                               rc03_body_len_ - rc03_bit_body_offset_);
    bits.set_position(rc03_bit_byte_index_, rc03_bit_index_);

    for (uint32_t i = 0; i < window_count; ++i) {
        const uint64_t sample_index = rc03_samples_decoded_;
        int64_t raw = 0;
        if (sample_index == 0) {
            raw = report_series_read_le_i16(rc03_body_);
            rc03_prev2_ = raw;
        } else if (sample_index == 1) {
            raw = report_series_read_le_i16(rc03_body_ + 2);
            rc03_prev1_ = raw;
        } else {
            uint64_t encoded = 0;
            if (!report_series_read_rice(bits, rc03_rice_m_, encoded)) {
                report_series_set_error(error,
                                        error_len,
                                        "rc03_bitstream_short");
                return ReportSeriesReadResult::Failed;
            }

            const int64_t delta2 = report_series_zigzag_decode(encoded);
            raw = 2 * rc03_prev1_ - rc03_prev2_ + delta2;
            rc03_prev2_ = rc03_prev1_;
            rc03_prev1_ = raw;
        }

        if (!report_series_append_scaled_value_le(output,
                                                   raw,
                                                   rc03_scale_)) {
            report_series_set_error(error, error_len, "series_append_failed");
            return ReportSeriesReadResult::Failed;
        }

        rc03_samples_decoded_++;
    }
    bits.position(rc03_bit_byte_index_, rc03_bit_index_);

    if (output.size() != payload_size) {
        report_series_set_error(error, error_len, "series_v2_build_failed");
        return ReportSeriesReadResult::Failed;
    }

    const uint64_t chunk_start =
        rc03_start_ms_ + first_sample * rc03_interval_ms_;
    const uint64_t chunk_end = chunk_start +
        static_cast<uint64_t>(window_count) * rc03_interval_ms_;
    chunk.source = source_;
    chunk.kind = ReportParsedChunkKind::Series;
    chunk.name = signal_name_;
    chunk.start_ms = static_cast<int64_t>(chunk_start);
    chunk.end_ms = static_cast<int64_t>(chunk_end);
    chunk.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    chunk.record_count = window_count;
    chunk.payload = output.data();
    chunk.payload_len = output.size();

    if (rc03_samples_decoded_ == rc03_sample_count_) {
        rc03_record_active_ = false;
    }
    report_series_set_error(error, error_len, "");
    return ReportSeriesReadResult::Chunk;
}

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

    ReportSeriesSpoolReader reader;
    if (!reader.begin(result, source, error, error_len)) return false;

    ReportSpoolBuffer output;
    while (true) {
        ReportParsedChunk chunk;
        const ReportSeriesReadResult read_result =
            reader.next(chunk, output, error, error_len);
        if (read_result == ReportSeriesReadResult::Done) return true;
        if (read_result == ReportSeriesReadResult::Failed) return false;

        if (!callback(context, chunk)) {
            if (!error || !error[0]) {
                report_series_set_error(error,
                                        error_len,
                                        source == ReportSourceId::TherapyOneMinute
                                            ? "therapy_series_chunk_rejected"
                                            : "series_chunk_rejected");
            }
            return false;
        }
    }
}

}  // namespace aircannect
