#pragma once

#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_proto.h"

namespace aircannect {

enum class ReportEdfTimezoneSource : uint8_t {
    None,
    Summary,
    PosixRule,
    CurrentDeviceOffset,
};

struct ReportEdfTimezoneResolution {
    bool valid = false;
    int32_t offset_minutes = 0;
    ReportEdfTimezoneSource source = ReportEdfTimezoneSource::None;
};

bool report_edf_summary_matches_sleep_day(
    const ReportSummaryRecord &summary,
    const char *sleep_day);

bool report_edf_resolve_session_timezone(
    EdfReportSessionDescriptor &session,
    const ReportSummaryRecord *matching_summary,
    bool current_offset_valid,
    int32_t current_offset_minutes,
    ReportEdfTimezoneResolution &out);

}  // namespace aircannect
