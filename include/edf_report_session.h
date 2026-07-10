#pragma once

#include <stddef.h>

#include "edf_report_catalog.h"

namespace aircannect {

bool edf_session_has_report_numeric(
    const EdfReportSessionDescriptor &session);
bool edf_session_has_report_annotation(
    const EdfReportSessionDescriptor &session);
bool edf_report_session_reportable(
    const EdfReportSessionDescriptor &session);

bool edf_session_annotation_matches_numeric(
    const EdfReportSessionDescriptor &numeric_session,
    const EdfReportSessionDescriptor &annotation_session);

void edf_report_session_refresh_bounds(
    EdfReportSessionDescriptor &session);
bool edf_report_session_apply_timezone_offset(
    EdfReportSessionDescriptor &session,
    int32_t timezone_offset_minutes);
void normalize_edf_report_sessions(EdfReportSessionDescriptor *sessions,
                                   size_t &session_count);

}  // namespace aircannect
