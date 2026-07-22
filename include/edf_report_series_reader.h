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
    EdfSignalScale signal_scale;
    EdfReportSignalMapping mapping;
    int64_t header_start_ms = 0;
    uint32_t record_duration_ms = 0;
    uint32_t record_size = 0;
    uint32_t complete_records = 0;
    uint32_t signal_index = 0;
};

struct EdfReportSeriesDecodeStats {
    uint32_t samples_seen = 0;
    uint32_t samples_missing = 0;
    uint32_t samples_out_of_range = 0;
    uint32_t samples_emitted = 0;
};

using EdfReportSeriesSampleCallback =
    bool (*)(void *context, const ReportSeriesSample &sample);

EdfReportSeriesStatus edf_report_series_decoder_init(
    const EdfReportFileDescriptor &file,
    const uint8_t *header,
    size_t header_size,
    ReportSignalId signal,
    bool require_primary,
    EdfReportSeriesDecoder &out);

EdfReportSeriesStatus edf_report_series_decoder_init(
    const EdfReportSignalLayout &layout,
    int64_t header_start_ms,
    uint32_t record_duration_ms,
    uint32_t record_size,
    uint32_t complete_records,
    EdfReportSeriesDecoder &out);

EdfReportSeriesStatus edf_report_decode_series_record(
    const EdfReportSeriesDecoder &decoder,
    const uint8_t *record,
    size_t record_size,
    uint32_t record_index,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSeriesSampleCallback callback,
    void *context,
    EdfReportSeriesDecodeStats &stats);

}  // namespace aircannect
