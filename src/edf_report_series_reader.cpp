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

bool mapped_signal_matches(const EdfReportFileDescriptor &file,
                           uint32_t signal_index,
                           ReportSignalId signal,
                           bool require_primary,
                           EdfReportSignalMapping &mapping) {
    if (signal_index >= file.signal_count) return false;
    if (!edf_report_signal_mapping(file.inventory.kind,
                                   file.signals[signal_index].label,
                                   mapping)) {
        return false;
    }
    if (mapping.signal != signal) return false;
    return !require_primary || mapping.primary;
}

bool find_mapped_signal(const EdfReportFileDescriptor &file,
                        ReportSignalId signal,
                        bool require_primary,
                        uint32_t &signal_index,
                        EdfReportSignalMapping &mapping) {
    bool found_fallback = false;
    uint32_t fallback_index = 0;
    EdfReportSignalMapping fallback_mapping;

    for (uint32_t i = 0; i < file.signal_count; ++i) {
        EdfReportSignalMapping candidate;
        if (!mapped_signal_matches(file,
                                   i,
                                   signal,
                                   false,
                                   candidate)) {
            continue;
        }
        if (candidate.primary) {
            signal_index = i;
            mapping = candidate;
            return true;
        }
        if (!found_fallback) {
            found_fallback = true;
            fallback_index = i;
            fallback_mapping = candidate;
        }
    }

    if (!require_primary && found_fallback) {
        signal_index = fallback_index;
        mapping = fallback_mapping;
        return true;
    }
    return false;
}

int32_t physical_to_milli(float value) {
    const long scaled = lroundf(value * 1000.0f);
    if (scaled < INT32_MIN) return INT32_MIN;
    if (scaled > INT32_MAX) return INT32_MAX;
    return static_cast<int32_t>(scaled);
}

}  // namespace

const char *edf_report_series_status_name(EdfReportSeriesStatus status) {
    switch (status) {
        case EdfReportSeriesStatus::Ok: return "ok";
        case EdfReportSeriesStatus::InvalidArgument:
            return "invalid_argument";
        case EdfReportSeriesStatus::SignalNotFound:
            return "signal_not_found";
        case EdfReportSeriesStatus::SignalNotMapped:
            return "signal_not_mapped";
        case EdfReportSeriesStatus::ScaleError: return "scale_error";
        case EdfReportSeriesStatus::RecordOutOfRange:
            return "record_out_of_range";
        case EdfReportSeriesStatus::RecordSizeMismatch:
            return "record_size_mismatch";
        case EdfReportSeriesStatus::CallbackRejected:
            return "callback_rejected";
        default:
            return "unknown";
    }
}

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
    if (!find_mapped_signal(file,
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

    out.file = file;
    out.signal_header = signal_header;
    out.signal_scale = signal_scale;
    out.mapping = mapping;
    out.signal_index = signal_index;
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
        decoder.file.status != EdfReportFileStatus::Ok ||
        decoder.file.record_duration_ms == 0 ||
        decoder.signal_header.samples_per_record == 0) {
        return EdfReportSeriesStatus::InvalidArgument;
    }
    if (record_index >= decoder.file.inventory.complete_records_from_size) {
        return EdfReportSeriesStatus::RecordOutOfRange;
    }
    if (record_size < decoder.file.inventory.header.record_size) {
        return EdfReportSeriesStatus::RecordSizeMismatch;
    }

    const int64_t record_start_ms =
        decoder.file.header_start_ms +
        static_cast<int64_t>(record_index) *
            static_cast<int64_t>(decoder.file.record_duration_ms);
    const int64_t record_end_ms =
        record_start_ms + static_cast<int64_t>(decoder.file.record_duration_ms);
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
             static_cast<int64_t>(decoder.file.record_duration_ms)) /
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
        if (edf_digital_sample_is_missing(decoder.signal_header, digital)) {
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
