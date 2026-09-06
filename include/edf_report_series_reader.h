#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_records.h"

namespace aircannect {

enum class EdfReportSeriesStatus : uint8_t {
    Ok,
    InvalidArgument,
    SignalNotFound,
    SignalNotMapped,
    ScaleError,
    RecordOutOfRange,
    RecordSizeMismatch,
    CallbackRejected,
};

struct EdfReportSeriesDecoder {
    EdfSignalHeader signal_header;
    // Original EDF units: physical = raw * scale + offset.
    EdfSignalScale signal_scale;
    EdfReportSignalMapping mapping;
    int64_t header_start_ms = 0;
    uint32_t record_duration_ms = 0;
    uint32_t record_size = 0;
    uint32_t complete_records = 0;
    uint32_t signal_index = 0;
};

// Borrowed view of the original little-endian EDF words for one clipped
// record. The view is valid only for the duration of the callback.
struct EdfReportSeriesSpan {
    const uint8_t *data = nullptr;
    uint32_t first_sample_index = 0;
    uint32_t sample_count = 0;
    uint32_t samples_per_record = 0;
    int64_t record_start_ms = 0;
    uint32_t record_duration_ms = 0;
    EdfSignalScale scale;

    bool valid() const;
    int16_t raw_at(uint32_t index) const;
    int64_t timestamp_at(uint32_t index) const;
    bool missing_at(uint32_t index) const;
    int32_t value_milli_at(uint32_t index) const;
};

int32_t edf_report_physical_value_milli(const EdfSignalScale &scale,
                                        int16_t raw);

using EdfReportSeriesSampleCallback =
    bool (*)(void *context, const ReportSeriesSample &sample);
using EdfReportSeriesSpanCallback =
    bool (*)(void *context, const EdfReportSeriesSpan &span);

EdfReportSeriesStatus edf_report_series_decoder_init(
    const EdfReportSignalLayout &layout,
    int64_t header_start_ms,
    uint32_t record_duration_ms,
    uint32_t record_size,
    uint32_t complete_records,
    EdfReportSeriesDecoder &out);

// Emits original digital samples alongside the legacy physical milli-values.
// Missing-sample filtering and the half-open output window apply to both.
EdfReportSeriesStatus edf_report_decode_series_record(
    const EdfReportSeriesDecoder &decoder,
    const uint8_t *record,
    size_t record_size,
    uint32_t record_index,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSeriesSampleCallback callback,
    void *context);

EdfReportSeriesStatus edf_report_decode_series_record_spans(
    const EdfReportSeriesDecoder &decoder,
    const uint8_t *record,
    size_t record_size,
    uint32_t record_index,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSeriesSpanCallback callback,
    void *context);

}  // namespace aircannect
