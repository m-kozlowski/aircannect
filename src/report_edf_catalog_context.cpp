#include "report_edf_catalog_context.h"

#include <algorithm>
#include "edf_report_catalog_job.h"
#include "report_edf_timezone.h"
#include "report_night_index.h"

namespace aircannect {

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

bool ReportEdfCatalogContext::resolve_session_timezone(
    EdfReportSessionDescriptor &session,
    const ReportSummaryRecord *matching_summary,
    int32_t &offset_minutes) const {
    int32_t current_offset_minutes = 0;
    const bool current_offset_valid =
        timezone_offset_minutes(current_offset_minutes);

    return report_edf_resolve_session_timezone(session,
                                               matching_summary,
                                               current_offset_valid,
                                               current_offset_minutes,
                                               offset_minutes);
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
    const size_t count = session_count();
    for (size_t i = 0; i < count &&
                       out_session_count < session_capacity; ++i) {
        EdfReportSessionDescriptor &session = sessions[out_session_count];
        if (!copy_session(i, session)) continue;

        const bool matches_sleep_day =
            have_target_sleep_day &&
            strcmp(session.sleep_day, target_sleep_day) == 0;
        if (have_target_sleep_day && !matches_sleep_day) continue;

        int32_t resolved_offset_minutes = 0;
        const ReportSummaryRecord *matching_summary =
            matches_sleep_day ? &night : nullptr;
        if (!resolve_session_timezone(session,
                                      matching_summary,
                                      resolved_offset_minutes)) {
            continue;
        }

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
        if (!edf_report_session_reportable(session)) continue;
        out_session_count++;
    }

    std::sort(sessions,
              sessions + out_session_count,
              [](const EdfReportSessionDescriptor &a,
                 const EdfReportSessionDescriptor &b) {
                  return a.earliest_header_start_ms <
                         b.earliest_header_start_ms;
              });
    if (out_session_count == 0 &&
        status.state == EdfReportCatalogState::Refreshing) {
        if (pending_out) *pending_out = true;
        return false;
    }

    return out_session_count > 0;
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
