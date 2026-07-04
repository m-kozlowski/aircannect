#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"

namespace aircannect {
namespace {

bool edf_session_same_identity(const EdfReportSessionDescriptor &a,
                               const EdfReportSessionDescriptor &b) {
    return strcmp(a.sleep_day, b.sleep_day) == 0 &&
           strcmp(a.session_stamp, b.session_stamp) == 0;
}

}  // namespace

bool ReportManager::edf_catalog_session_has_annotation_marker(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (!edf_catalog_ || !edf_session_has_report_numeric(session)) {
        return false;
    }
    if (edf_session_has_report_annotation(session)) return true;

    const size_t count = edf_catalog_->session_count();
    for (size_t i = 0; i < count; ++i) {
        if (!edf_catalog_->copy_session(i, scratch)) continue;
        if (edf_session_annotation_matches_numeric(session, scratch)) {
            return true;
        }
    }
    return false;
}

bool ReportManager::edf_catalog_annotation_has_numeric_session(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (!edf_catalog_ || !edf_session_has_report_annotation(session)) {
        return false;
    }
    if (edf_session_has_report_numeric(session)) return true;

    const size_t count = edf_catalog_->session_count();
    for (size_t i = 0; i < count; ++i) {
        if (!edf_catalog_->copy_session(i, scratch)) continue;
        if (edf_session_annotation_matches_numeric(scratch, session)) {
            return true;
        }
    }
    return false;
}

bool ReportManager::edf_catalog_session_reportable(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (edf_session_has_report_numeric(session)) {
        return edf_catalog_session_has_annotation_marker(session, scratch);
    }
    return edf_catalog_annotation_has_numeric_session(session, scratch);
}

bool ReportManager::collect_edf_sessions_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSessionDescriptor *sessions,
    size_t session_capacity,
    size_t &session_count,
    bool *pending_out) const {
    session_count = 0;
    if (pending_out) *pending_out = false;
    if (!edf_catalog_ || !sessions || session_capacity == 0 ||
        range_end_ms <= range_start_ms) {
        return false;
    }

    EdfReportCatalogStatus status;
    if (!edf_catalog_->status(status, 0) ||
        (status.state != EdfReportCatalogState::Ready &&
         status.sessions == 0)) {
        if (status.state != EdfReportCatalogState::Error) {
            (void)edf_catalog_->request_refresh();
            if (pending_out) *pending_out = true;
        }
        return false;
    }

    const size_t count = edf_catalog_->session_count();
    char target_sleep_day[9] = {};
    const bool have_target_sleep_day =
        report_summary_sleep_day_yyyymmdd(night,
                                          target_sleep_day,
                                          sizeof(target_sleep_day));
    EdfReportSessionDescriptor *marker_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!marker_scratch) {
        log_report_alloc_failed("edf_session_marker_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }

    for (size_t i = 0; i < count &&
                       session_count < session_capacity; ++i) {
        EdfReportSessionDescriptor &session = sessions[session_count];
        if (!edf_catalog_->copy_session(i, session)) continue;

        const bool matches_sleep_day =
            have_target_sleep_day &&
            strcmp(session.sleep_day, target_sleep_day) == 0;
        const bool matches_range =
            session.earliest_header_start_ms > 0 &&
            session.latest_header_end_ms > session.earliest_header_start_ms &&
            ranges_overlap(session.earliest_header_start_ms,
                           session.latest_header_end_ms,
                           range_start_ms,
                           range_end_ms);
        if (!matches_sleep_day && !matches_range) {
            continue;
        }
        if (!edf_catalog_session_reportable(session, *marker_scratch)) {
            continue;
        }
        session_count++;
    }
    Memory::free(marker_scratch);

    std::sort(sessions,
              sessions + session_count,
              [](const EdfReportSessionDescriptor &a,
                 const EdfReportSessionDescriptor &b) {
                  return a.earliest_header_start_ms <
                         b.earliest_header_start_ms;
              });
    if (!append_edf_sessions_for_selected_days(sessions,
                                               session_capacity,
                                               session_count)) {
        return false;
    }
    if (session_count == 0 &&
        status.state == EdfReportCatalogState::Refreshing) {
        if (pending_out) *pending_out = true;
        return false;
    }
    return session_count > 0;
}

bool ReportManager::append_edf_sessions_for_selected_days(
    EdfReportSessionDescriptor *sessions,
    size_t session_capacity,
    size_t &session_count) const {
    if (!edf_catalog_ || !sessions || session_count == 0 ||
        session_capacity == 0) {
        return true;
    }

    const size_t base_count = session_count;
    const size_t catalog_count = edf_catalog_->session_count();
    EdfReportSessionDescriptor *candidate =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!candidate) {
        log_report_alloc_failed("edf_event_session_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }

    EdfReportSessionDescriptor *marker_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!marker_scratch) {
        Memory::free(candidate);
        log_report_alloc_failed("edf_event_marker_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }

    for (size_t i = 0; i < catalog_count; ++i) {
        if (!edf_catalog_->copy_session(i, *candidate)) continue;

        bool selected_day = false;
        for (size_t base = 0; base < base_count; ++base) {
            if (strcmp(sessions[base].sleep_day, candidate->sleep_day) == 0) {
                selected_day = true;
                break;
            }
        }
        if (!selected_day) continue;
        if (!edf_catalog_session_reportable(*candidate, *marker_scratch)) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0; existing < session_count; ++existing) {
            if (edf_session_same_identity(sessions[existing], *candidate)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        if (session_count >= session_capacity) {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "EDF report session list full capacity=%u\n",
                      static_cast<unsigned>(session_capacity));
            break;
        }
        sessions[session_count++] = *candidate;
    }
    Memory::free(marker_scratch);
    Memory::free(candidate);

    merge_edf_annotation_sessions(sessions, session_count);

    std::sort(sessions,
              sessions + session_count,
              [](const EdfReportSessionDescriptor &a,
                 const EdfReportSessionDescriptor &b) {
                  if (strcmp(a.sleep_day, b.sleep_day) != 0) {
                      return strcmp(a.sleep_day, b.sleep_day) < 0;
                  }
                  return strcmp(a.session_stamp, b.session_stamp) < 0;
              });
    return true;
}

bool ReportManager::find_edf_sessions_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *pending_out) {
    result_edf_session_count_ = 0;
    if (pending_out) *pending_out = false;
    if (!ensure_result_edf_sessions()) return false;

    memset(result_edf_sessions_,
           0,
           AC_REPORT_EDF_SESSION_MAX *
               sizeof(EdfReportSessionDescriptor));
    return collect_edf_sessions_for_night(night,
                                          range_start_ms,
                                          range_end_ms,
                                          result_edf_sessions_,
                                          AC_REPORT_EDF_SESSION_MAX,
                                          result_edf_session_count_,
                                          pending_out);
}

}  // namespace aircannect
