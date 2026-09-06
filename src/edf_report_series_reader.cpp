#include "edf_report_series_reader.h"
#include "edf_bytes.h"

#include <algorithm>
#include <math.h>
#include <string.h>

namespace aircannect {
namespace {

bool ranges_overlap(int64_t start_a,
                    int64_t end_a,
                    int64_t start_b,
                    int64_t end_b) {
    return start_a < end_b && start_b < end_a;
}

int32_t physical_to_milli(float value) {
    const long scaled = lroundf(value * 1000.0f);
    if (scaled < INT32_MIN) return INT32_MIN;
    if (scaled > INT32_MAX) return INT32_MAX;
    return static_cast<int32_t>(scaled);
}

}  // namespace

bool EdfReportSeriesSpan::valid() const {
    return data && sample_count > 0 && samples_per_record > 0 &&
        first_sample_index <= samples_per_record &&
        sample_count <= samples_per_record - first_sample_index &&
        record_duration_ms > 0;
}

int16_t EdfReportSeriesSpan::raw_at(uint32_t index) const {
    return edf_read_i16_le_sample(data, index);
}

int64_t EdfReportSeriesSpan::timestamp_at(uint32_t index) const {
    const uint64_t sample_index =
        static_cast<uint64_t>(first_sample_index) + index;
    const uint64_t elapsed = sample_index * record_duration_ms /
        samples_per_record;
    return record_start_ms + static_cast<int64_t>(elapsed);
}

bool EdfReportSeriesSpan::missing_at(uint32_t index) const {
    return edf_digital_sample_is_missing(scale, raw_at(index));
}

int32_t EdfReportSeriesSpan::value_milli_at(uint32_t index) const {
    return edf_report_physical_value_milli(scale, raw_at(index));
}

int32_t edf_report_physical_value_milli(const EdfSignalScale &scale,
                                        int16_t raw) {
    return physical_to_milli(edf_scale_digital_sample(scale, raw));
}

EdfReportSeriesStatus edf_report_series_decoder_init(
    const EdfReportSignalLayout &layout,
    int64_t header_start_ms,
    uint32_t record_duration_ms,
    uint32_t record_size,
    uint32_t complete_records,
    EdfReportSeriesDecoder &out) {
    out = {};
    const uint64_t signal_end =
        static_cast<uint64_t>(layout.byte_offset_in_record) +
        static_cast<uint64_t>(layout.samples_per_record) * 2u;
    if (header_start_ms <= 0 || record_duration_ms == 0 || record_size == 0 ||
        layout.samples_per_record == 0 || signal_end > record_size ||
        layout.scale.digital_max <= layout.scale.digital_min ||
        !isfinite(layout.scale.scale) || !isfinite(layout.scale.offset) ||
        layout.scale.scale <= 0.0f) {
        return EdfReportSeriesStatus::InvalidArgument;
    }

    out.signal_header.samples_per_record = layout.samples_per_record;
    out.signal_header.byte_offset_in_record = layout.byte_offset_in_record;
    out.signal_scale = layout.scale;
    out.mapping.signal = layout.signal;
    out.mapping.source = layout.source;
    out.mapping.sample_interval_ms = layout.sample_interval_ms;
    out.mapping.primary = layout.primary;
    out.header_start_ms = header_start_ms;
    out.record_duration_ms = record_duration_ms;
    out.record_size = record_size;
    out.complete_records = complete_records;
    return EdfReportSeriesStatus::Ok;
}

EdfReportSeriesStatus edf_report_decode_series_record(
    const EdfReportSeriesDecoder &decoder,
    const uint8_t *record,
    size_t record_size,
    uint32_t record_index,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSeriesSampleCallback callback,
    void *context) {
    struct LegacyContext {
        EdfReportSeriesSampleCallback callback = nullptr;
        void *context = nullptr;
    } legacy{callback, context};

    const auto emit_legacy = [](void *legacy_context,
                                const EdfReportSeriesSpan &span) {
        LegacyContext *state = static_cast<LegacyContext *>(legacy_context);
        if (!state || !state->callback) return false;
        for (uint32_t i = 0; i < span.sample_count; ++i) {
            if (span.missing_at(i)) continue;

            ReportSeriesSample sample;
            sample.timestamp_ms = span.timestamp_at(i);
            sample.raw = span.raw_at(i);
            sample.raw_valid = true;
            sample.value_milli = span.value_milli_at(i);
            if (!state->callback(state->context, sample)) return false;
        }
        return true;
    };

    return edf_report_decode_series_record_spans(
        decoder, record, record_size, record_index, range_start_ms,
        range_end_ms, emit_legacy, &legacy);
}

EdfReportSeriesStatus edf_report_decode_series_record_spans(
    const EdfReportSeriesDecoder &decoder,
    const uint8_t *record,
    size_t record_size,
    uint32_t record_index,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSeriesSpanCallback callback,
    void *context) {
    if (!record || !callback || range_end_ms <= range_start_ms ||
        decoder.record_duration_ms == 0 || decoder.record_size == 0 ||
        decoder.signal_header.samples_per_record == 0) {
        return EdfReportSeriesStatus::InvalidArgument;
    }
    if (record_index >= decoder.complete_records) {
        return EdfReportSeriesStatus::RecordOutOfRange;
    }
    if (record_size < decoder.record_size) {
        return EdfReportSeriesStatus::RecordSizeMismatch;
    }

    const int64_t record_start_ms =
        decoder.header_start_ms +
        static_cast<int64_t>(record_index) *
            static_cast<int64_t>(decoder.record_duration_ms);
    const int64_t record_end_ms =
        record_start_ms + static_cast<int64_t>(decoder.record_duration_ms);
    if (!ranges_overlap(record_start_ms,
                        record_end_ms,
                        range_start_ms,
                        range_end_ms)) {
        return EdfReportSeriesStatus::Ok;
    }

    const uint32_t samples_per_record =
        decoder.signal_header.samples_per_record;
    const int64_t clipped_start_ms =
        std::max(record_start_ms, range_start_ms);
    const int64_t clipped_end_ms =
        std::min(record_end_ms, range_end_ms);
    const uint64_t start_delta = static_cast<uint64_t>(
        clipped_start_ms - record_start_ms);
    const uint64_t end_delta = static_cast<uint64_t>(
        clipped_end_ms - record_start_ms);
    const uint64_t sample_count_u64 = samples_per_record;
    const uint64_t duration = decoder.record_duration_ms;
    const uint64_t first_index_u64 = start_delta == 0
        ? 0
        : (start_delta * sample_count_u64 + duration - 1) / duration;
    const uint64_t end_index_u64 = end_delta == 0
        ? 0
        : (end_delta * sample_count_u64 + duration - 1) / duration;
    const uint32_t first_sample_index = static_cast<uint32_t>(
        std::min(first_index_u64, sample_count_u64));
    const uint32_t end_sample_index = static_cast<uint32_t>(
        std::min(end_index_u64, sample_count_u64));
    const uint32_t sample_count = end_sample_index > first_sample_index
        ? end_sample_index - first_sample_index
        : 0;

    if (sample_count == 0) return EdfReportSeriesStatus::Ok;

    const size_t byte_offset = static_cast<size_t>(
        decoder.signal_header.byte_offset_in_record) +
        static_cast<size_t>(first_sample_index) * 2;
    const size_t byte_length = static_cast<size_t>(sample_count) * 2;
    if (byte_offset > record_size || byte_length > record_size - byte_offset) {
        return EdfReportSeriesStatus::RecordSizeMismatch;
    }

    EdfReportSeriesSpan span;
    span.data = record + byte_offset;
    span.first_sample_index = first_sample_index;
    span.sample_count = sample_count;
    span.samples_per_record = samples_per_record;
    span.record_start_ms = record_start_ms;
    span.record_duration_ms = decoder.record_duration_ms;
    span.scale = decoder.signal_scale;
    if (!callback(context, span)) {
        return EdfReportSeriesStatus::CallbackRejected;
    }

    return EdfReportSeriesStatus::Ok;
}

}  // namespace aircannect
