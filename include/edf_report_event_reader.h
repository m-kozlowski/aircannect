#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_records.h"

namespace aircannect {

enum class EdfReportEventStatus : uint8_t {
    Ok,
    InvalidArgument,
    CrcError,
    ParseError,
    CallbackRejected,
};

struct EdfReportEventDecodeStats {
    uint32_t annotations_seen = 0;
    uint32_t events_emitted = 0;
    uint32_t unsupported_labels = 0;
};

struct EdfReportEventDecodeContext {
    bool csr_open = false;
    int64_t csr_start_ms = 0;
};

using EdfReportEventCallback =
    bool (*)(void *context, const ReportEventRecord &event);

const char *edf_report_event_status_name(EdfReportEventStatus status);

EdfReportEventStatus edf_report_decode_annotation_record(
    const EdfReportFileDescriptor &file,
    const uint8_t *record,
    size_t record_size,
    bool verify_crc,
    EdfReportEventCallback callback,
    void *context,
    EdfReportEventDecodeStats &stats,
    EdfReportEventDecodeContext *decode_context = nullptr);

}  // namespace aircannect
