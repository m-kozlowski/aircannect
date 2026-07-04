#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

#include "edf_report_data_reader.h"

namespace aircannect {

EdfReportDataReadStatus edf_report_stream_series_batch_from_header(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry *entries,
    size_t entry_count,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta *metas,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchSampleCallback callback,
    void *context);

EdfReportDataReadStatus edf_report_stream_series_batch_plot_from_header(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry *entries,
    size_t entry_count,
    const EdfReportSeriesPlotConfig *configs,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta *metas,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchPlotCallback callback,
    void *context);

}  // namespace aircannect
