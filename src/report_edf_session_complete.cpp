#include "report_manager.h"

#include <algorithm>

#include "edf_report_data_plan.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"

namespace aircannect {
namespace {

size_t collect_required_ranges_from_session_ranges(
    const ReportSessionRange *session_ranges,
    size_t session_range_count,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportRequiredRange *required_ranges,
    size_t max_ranges) {
    if (!session_ranges || !required_ranges || max_ranges == 0 ||
        range_end_ms <= range_start_ms) {
        return 0;
    }

    size_t required_range_count = 0;
    for (size_t i = 0; i < session_range_count &&
                       required_range_count < max_ranges; ++i) {
        if (!ranges_overlap(session_ranges[i].start_ms,
                            session_ranges[i].end_ms,
                            range_start_ms,
                            range_end_ms)) {
            continue;
        }

        EdfReportRequiredRange &range = required_ranges[required_range_count];
        range.start_ms = std::max(session_ranges[i].start_ms, range_start_ms);
        range.end_ms = std::min(session_ranges[i].end_ms, range_end_ms);
        if (range.end_ms > range.start_ms) required_range_count++;
    }

    return required_range_count;
}

size_t collect_required_edf_ranges(const ReportSummaryRecord &night,
                                   int64_t range_start_ms,
                                   int64_t range_end_ms,
                                   EdfReportRequiredRange *required_ranges,
                                   size_t max_ranges) {
    if (!required_ranges || max_ranges == 0 ||
        range_end_ms <= range_start_ms) {
        return 0;
    }

    ReportSessionRange session_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t session_range_count =
        collect_session_ranges(night,
                               session_ranges,
                               AC_REPORT_SUMMARY_SESSION_MAX);
    return collect_required_ranges_from_session_ranges(session_ranges,
                                                       session_range_count,
                                                       range_start_ms,
                                                       range_end_ms,
                                                       required_ranges,
                                                       max_ranges);
}

}  // namespace

bool ReportManager::edf_report_complete_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *pending_out) const {
    EdfReportSessionDescriptor *sessions =
        static_cast<EdfReportSessionDescriptor *>(Memory::alloc_large(
            AC_REPORT_EDF_SESSION_MAX *
            sizeof(EdfReportSessionDescriptor),
            false));
    if (!sessions) {
        log_report_alloc_failed(
            "edf_report_session_snapshot",
            AC_REPORT_EDF_SESSION_MAX *
                sizeof(EdfReportSessionDescriptor));
        if (pending_out) *pending_out = false;
        return false;
    }

    size_t session_count = 0;
    if (!collect_edf_sessions_for_night(night,
                                        range_start_ms,
                                        range_end_ms,
                                        sessions,
                                        AC_REPORT_EDF_SESSION_MAX,
                                        session_count,
                                        pending_out)) {
        Memory::free(sessions);
        return false;
    }

    EdfReportRequiredRange required_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t required_range_count =
        collect_required_edf_ranges(night,
                                    range_start_ms,
                                    range_end_ms,
                                    required_ranges,
                                    AC_REPORT_SUMMARY_SESSION_MAX);

    const bool complete = edf_report_plan_covers_report(sessions,
                                                        session_count,
                                                        required_ranges,
                                                        required_range_count);
    Memory::free(sessions);
    return complete;
}

bool ReportManager::edf_report_complete_for_indexed_night(
    const ReportIndexedNight &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *pending_out) const {
    EdfReportSessionDescriptor *sessions =
        static_cast<EdfReportSessionDescriptor *>(Memory::alloc_large(
            AC_REPORT_EDF_SESSION_MAX *
            sizeof(EdfReportSessionDescriptor),
            false));
    if (!sessions) {
        log_report_alloc_failed(
            "edf_report_session_snapshot",
            AC_REPORT_EDF_SESSION_MAX *
                sizeof(EdfReportSessionDescriptor));
        if (pending_out) *pending_out = false;
        return false;
    }

    size_t session_count = 0;
    if (!collect_edf_sessions_for_night(night.summary,
                                        range_start_ms,
                                        range_end_ms,
                                        sessions,
                                        AC_REPORT_EDF_SESSION_MAX,
                                        session_count,
                                        pending_out)) {
        Memory::free(sessions);
        return false;
    }

    EdfReportRequiredRange required_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    ReportSessionRange source_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t source_range_count =
        collect_indexed_night_report_ranges(night,
                                            source_ranges,
                                            AC_REPORT_SUMMARY_SESSION_MAX);
    const size_t required_range_count =
        collect_required_ranges_from_session_ranges(
            source_ranges,
            source_range_count,
            range_start_ms,
            range_end_ms,
            required_ranges,
            AC_REPORT_SUMMARY_SESSION_MAX);

    const bool complete = edf_report_plan_covers_report(sessions,
                                                        session_count,
                                                        required_ranges,
                                                        required_range_count);
    Memory::free(sessions);
    return complete;
}

bool ReportManager::edf_report_complete_for_night_sessions(
    const ReportSummaryRecord &night) const {
    int64_t span_start_ms = 0;
    int64_t span_end_ms = 0;
    return night_data_span(night, span_start_ms, span_end_ms) &&
           edf_report_complete_for_night(night,
                                         span_start_ms,
                                         span_end_ms);
}

}  // namespace aircannect
