#include "report_result_build_service.h"

#include <string.h>

#include "report_edf_catalog_context.h"

namespace aircannect {

bool ReportResultBuildService::find_edf_sessions_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *pending_out) {
    runtime_.scratch().edf_session_count() = 0;
    if (pending_out) *pending_out = false;
    if (!runtime_.ensure_edf_sessions()) return false;

    memset(runtime_.scratch().edf_sessions(),
           0,
           AC_REPORT_EDF_SESSION_MAX *
               sizeof(EdfReportSessionDescriptor));
    return edf_catalog_.collect_sessions_for_night(
        night,
        range_start_ms,
        range_end_ms,
        runtime_.scratch().edf_sessions(),
        AC_REPORT_EDF_SESSION_MAX,
        runtime_.scratch().edf_session_count(),
        pending_out);
}

}  // namespace aircannect
