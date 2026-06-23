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

const char *edf_report_data_read_status_name(
    EdfReportDataReadStatus status);

EdfReportDataReadStatus edf_report_read_entry_payload(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry &entry,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats);

}  // namespace aircannect
