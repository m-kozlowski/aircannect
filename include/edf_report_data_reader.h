#pragma once

#include <stdint.h>

#include "edf_report_data_plan.h"
#include "report_store.h"

namespace aircannect {

enum class EdfReportDataReadStatus : uint8_t {
    Ok,
    InvalidArgument,
    FileOpenFailed,
    HeaderReadFailed,
    HeaderParseFailed,
    RecordReadFailed,
    DecodeFailed,
    PayloadFull,
    CallbackRejected,
};

enum class EdfReportBatchPollResult : uint8_t {
    Pending,
    Complete,
    Failed,
};

struct EdfReportDataReadStats {
    uint32_t records_read = 0;
    uint32_t samples_seen = 0;
    uint32_t samples_missing = 0;
    uint32_t samples_out_of_range = 0;
    uint32_t samples_emitted = 0;
    uint32_t annotations_seen = 0;
    uint32_t events_emitted = 0;
    uint32_t unsupported_event_labels = 0;
};

using EdfReportSeriesBatchSampleCallback =
    bool (*)(void *context, size_t item_index,
             const ReportSeriesSample &sample);

struct EdfReportPlotRange {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

struct EdfReportSeriesPlotConfig {
    const EdfReportPlotRange *ranges = nullptr;
    size_t range_count = 0;
    int64_t plot_start_ms = 0;
    uint32_t bucket_ms = 1;
    uint32_t gap_threshold_ms = 5000;
    int32_t value_multiplier = 1;
};

struct EdfReportSeriesPlotPoint {
    bool gap = false;
    int64_t timestamp_ms = 0;
    int32_t value_milli = 0;
};

using EdfReportSeriesBatchPlotCallback =
    bool (*)(void *context, size_t item_index,
             const EdfReportSeriesPlotPoint &point);

const char *edf_report_data_read_status_name(
    EdfReportDataReadStatus status);

bool edf_report_signal_uses_edge_zero_padding(ReportSignalId signal);

EdfReportDataReadStatus edf_report_read_entry_payload(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry &entry,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats);

EdfReportDataReadStatus edf_report_for_each_entry_series_sample(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry &entry,
    ReportStoreChunkMeta &meta,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    ReportSeriesSampleCallback callback,
    void *context);

}  // namespace aircannect
