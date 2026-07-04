#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

#include "edf_report_data_reader.h"

namespace aircannect {

const EdfReportSessionFileDescriptor *edf_report_data_entry_file(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry &entry);

EdfReportDataReadStatus edf_report_data_read_exact(File &file,
                                                   uint8_t *buffer,
                                                   size_t len);

EdfReportDataReadStatus edf_report_data_open_file(
    const EdfReportSessionFileDescriptor &session_file,
    File &file);

void edf_report_data_close_file(File &file);

EdfReportDataReadStatus edf_report_data_read_header(
    const EdfReportSessionFileDescriptor &session_file,
    EdfReportFileDescriptor &file_desc,
    uint8_t *&header,
    size_t &header_size,
    File &file);

EdfReportDataReadStatus edf_report_data_seek_record(
    File &file,
    const EdfReportSessionFileDescriptor &session_file,
    uint32_t record_index);

}  // namespace aircannect
