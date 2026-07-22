#include "edf_report_series_reader.h"

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

EdfReportSeriesStatus edf_report_series_decoder_init(
    const EdfReportFileDescriptor &file,
    const uint8_t *header,
    size_t header_size,
    ReportSignalId signal,
    bool require_primary,
    EdfReportSeriesDecoder &out) {
    out = {};
    if (file.status != EdfReportFileStatus::Ok || !header ||
        header_size < file.inventory.header.header_size ||
        file.record_duration_ms == 0 || file.inventory.header.record_size == 0) {
        return EdfReportSeriesStatus::InvalidArgument;
    }

    uint32_t signal_index = 0;
    EdfReportSignalMapping mapping;
    if (!edf_report_file_find_signal_mapping(file,
                                             signal,
                                             require_primary,
                                             signal_index,
                                             mapping)) {
        return EdfReportSeriesStatus::SignalNotFound;
    }

    EdfSignalHeader signal_header;
    if (!edf_parse_signal_header(header,
                                 header_size,
                                 signal_index,
                                 signal_header)) {
        return EdfReportSeriesStatus::SignalNotMapped;
    }

    EdfSignalScale signal_scale;
    if (!edf_parse_signal_scale(signal_header, signal_scale)) {
        return EdfReportSeriesStatus::ScaleError;
    }

    EdfReportSignalLayout layout;
    layout.scale = signal_scale;
    layout.samples_per_record = signal_header.samples_per_record;
    layout.byte_offset_in_record = signal_header.byte_offset_in_record;
    layout.sample_interval_ms = mapping.sample_interval_ms;
    layout.signal = mapping.signal;
    layout.source = mapping.source;
    layout.primary = mapping.primary;

    const EdfReportSeriesStatus status = edf_report_series_decoder_init(
        layout,
        file.header_start_ms,
        file.record_duration_ms,
        file.inventory.header.record_size,
        file.inventory.complete_records_from_size,
        out);
    if (status != EdfReportSeriesStatus::Ok) return status;
    out.signal_index = signal_index;
    return EdfReportSeriesStatus::Ok;
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
    void *context,
    EdfReportSeriesDecodeStats &stats) {
    stats = {};
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
        stats.samples_out_of_range = decoder.signal_header.samples_per_record;
        return EdfReportSeriesStatus::Ok;
    }

    for (uint32_t i = 0; i < decoder.signal_header.samples_per_record; ++i) {
        stats.samples_seen++;
        const int64_t sample_ms =
            record_start_ms +
            (static_cast<int64_t>(i) *
             static_cast<int64_t>(decoder.record_duration_ms)) /
                static_cast<int64_t>(
                    decoder.signal_header.samples_per_record);
        if (sample_ms < range_start_ms || sample_ms >= range_end_ms) {
            stats.samples_out_of_range++;
            continue;
        }

        int16_t digital = 0;
        if (!edf_decode_signal_digital_sample(decoder.signal_header,
                                              record,
                                              record_size,
                                              i,
                                              digital)) {
            return EdfReportSeriesStatus::RecordSizeMismatch;
        }
        if (edf_digital_sample_is_missing(decoder.signal_scale, digital)) {
            stats.samples_missing++;
            continue;
        }

        ReportSeriesSample sample;
        sample.timestamp_ms = sample_ms;
        sample.value_milli = physical_to_milli(
            edf_scale_digital_sample(decoder.signal_scale, digital));
        if (!callback(context, sample)) {
            return EdfReportSeriesStatus::CallbackRejected;
        }
        stats.samples_emitted++;
    }
    return EdfReportSeriesStatus::Ok;
}

}  // namespace aircannect
