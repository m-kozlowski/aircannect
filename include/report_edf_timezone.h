#pragma once

#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_proto.h"

namespace aircannect {

bool report_edf_resolve_session_timezone(
    EdfReportSessionDescriptor &session,
    const ReportSummaryRecord *matching_summary,
    bool current_offset_valid,
    int32_t current_offset_minutes,
    int32_t &resolved_offset_minutes);

}  // namespace aircannect
