#pragma once

#include <FS.h>

#include "edf_report_data_reader.h"

namespace aircannect {

EdfReportDataReadStatus edf_report_read_event_entry_payload(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    File &file,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats);

}  // namespace aircannect
