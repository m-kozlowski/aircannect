#include "report_edf_catalog_context.h"

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

void ReportEdfCatalogContext::set_catalog(EdfReportCatalogJob *catalog) {
    catalog_ = catalog;
}

bool ReportEdfCatalogContext::status(EdfReportCatalogStatus &out,
                                      uint32_t timeout_ms) const {
    return catalog_ && catalog_->status(out, timeout_ms);
}

bool ReportEdfCatalogContext::request_refresh() const {
    return catalog_ && catalog_->request_refresh();
}

bool ReportEdfCatalogContext::pending_or_request_refresh() const {
    if (!catalog_) return false;

    EdfReportCatalogStatus status;
    if (!this->status(status, 0)) return true;
    if (status.state == EdfReportCatalogState::Ready ||
        status.state == EdfReportCatalogState::Error) {
        return false;
    }
    if (status.sessions > 0) return false;

    (void)request_refresh();
    return true;
}

bool ReportEdfCatalogContext::timezone_offset_minutes(int32_t &out) const {
    return catalog_ && catalog_->timezone_offset_minutes(out);
}

size_t ReportEdfCatalogContext::session_count() const {
    return catalog_ ? catalog_->session_count() : 0;
}

bool ReportEdfCatalogContext::copy_session(
    size_t index,
    EdfReportSessionDescriptor &out) const {
    return catalog_ && catalog_->copy_session(index, out);
}

bool ReportEdfCatalogContext::session_has_annotation_marker(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (!catalog_ || !edf_session_has_report_numeric(session)) {
        return false;
    }
    if (edf_session_has_report_annotation(session)) return true;

    const size_t count = session_count();
    for (size_t i = 0; i < count; ++i) {
        if (!copy_session(i, scratch)) continue;
        if (edf_session_annotation_matches_numeric(session, scratch)) {
            return true;
        }
    }

    return false;
}

bool ReportEdfCatalogContext::annotation_has_numeric_session(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (!catalog_ || !edf_session_has_report_annotation(session)) {
        return false;
    }
    if (edf_session_has_report_numeric(session)) return true;

    const size_t count = session_count();
    for (size_t i = 0; i < count; ++i) {
        if (!copy_session(i, scratch)) continue;
        if (edf_session_annotation_matches_numeric(scratch, session)) {
            return true;
        }
    }

    return false;
}

bool ReportEdfCatalogContext::session_reportable(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (edf_session_has_report_numeric(session)) {
        return session_has_annotation_marker(session, scratch);
    }

    return annotation_has_numeric_session(session, scratch);
}

bool ReportEdfCatalogContext::collect_sessions_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSessionDescriptor *sessions,
    size_t session_capacity,
    size_t &out_session_count,
    bool *pending_out) const {
    out_session_count = 0;
    if (pending_out) *pending_out = false;
    if (!catalog_ || !sessions || session_capacity == 0 ||
        range_end_ms <= range_start_ms) {
        return false;
    }

    EdfReportCatalogStatus status;
    if (!this->status(status, 0) ||
        (status.state != EdfReportCatalogState::Ready &&
         status.sessions == 0)) {
        if (status.state != EdfReportCatalogState::Error) {
            (void)request_refresh();
            if (pending_out) *pending_out = true;
        }
        return false;
    }

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

    const size_t count = session_count();
    for (size_t i = 0; i < count &&
                       out_session_count < session_capacity; ++i) {
        EdfReportSessionDescriptor &session = sessions[out_session_count];
        if (!copy_session(i, session)) continue;

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
        if (!session_reportable(session, *marker_scratch)) {
            continue;
        }
        out_session_count++;
    }
    Memory::free(marker_scratch);

    std::sort(sessions,
              sessions + out_session_count,
              [](const EdfReportSessionDescriptor &a,
                 const EdfReportSessionDescriptor &b) {
                  return a.earliest_header_start_ms <
                         b.earliest_header_start_ms;
              });
    if (!append_sessions_for_selected_days(sessions,
                                           session_capacity,
                                           out_session_count)) {
        return false;
    }
    if (out_session_count == 0 &&
        status.state == EdfReportCatalogState::Refreshing) {
        if (pending_out) *pending_out = true;
        return false;
    }

    return out_session_count > 0;
}

bool ReportEdfCatalogContext::append_sessions_for_selected_days(
    EdfReportSessionDescriptor *sessions,
    size_t session_capacity,
    size_t &out_session_count) const {
    if (!catalog_ || !sessions || out_session_count == 0 ||
        session_capacity == 0) {
        return true;
    }

    const size_t base_count = out_session_count;
    const size_t catalog_count = session_count();
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
        if (!copy_session(i, *candidate)) continue;

        bool selected_day = false;
        for (size_t base = 0; base < base_count; ++base) {
            if (strcmp(sessions[base].sleep_day, candidate->sleep_day) == 0) {
                selected_day = true;
                break;
            }
        }
        if (!selected_day) continue;
        if (!session_reportable(*candidate, *marker_scratch)) continue;

        bool duplicate = false;
        for (size_t existing = 0; existing < out_session_count; ++existing) {
            if (edf_session_same_identity(sessions[existing], *candidate)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        if (out_session_count >= session_capacity) {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "EDF report session list full capacity=%u\n",
                      static_cast<unsigned>(session_capacity));
            break;
        }
        sessions[out_session_count++] = *candidate;
    }
    Memory::free(marker_scratch);
    Memory::free(candidate);

    merge_edf_annotation_sessions(sessions, out_session_count);

    std::sort(sessions,
              sessions + out_session_count,
              [](const EdfReportSessionDescriptor &a,
                 const EdfReportSessionDescriptor &b) {
                  if (strcmp(a.sleep_day, b.sleep_day) != 0) {
                      return strcmp(a.sleep_day, b.sleep_day) < 0;
                  }
                  return strcmp(a.session_stamp, b.session_stamp) < 0;
              });
    return true;
}

bool ReportEdfCatalogContext::ready_refresh_changed(
    EdfReportCatalogStatus &out) const {
    if (!status(out, 0)) return false;
    if (out.state != EdfReportCatalogState::Ready) return false;
    if (out.refresh_id == 0) return false;

    return out.refresh_id != summary_refresh_id_;
}

void ReportEdfCatalogContext::mark_summary_published(uint32_t refresh_id) {
    summary_refresh_id_ = refresh_id;
}

}  // namespace aircannect
