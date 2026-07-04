#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

#include "edf_report_data_reader.h"

namespace aircannect {

EdfReportDataReadStatus edf_report_read_series_entry_payload(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats);

EdfReportDataReadStatus edf_report_emit_series_entry_samples(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    File &file,
    ReportStoreChunkMeta &meta,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    ReportSeriesSampleCallback callback,
    void *context);

}  // namespace aircannect
