#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_night_index.h"

namespace aircannect {

class EdfReportCatalogJob;
struct EdfReportCatalogStatus;

class ReportEdfCatalogContext {
public:
    void set_catalog(EdfReportCatalogJob *catalog);

    explicit operator bool() const { return present(); }
    bool present() const { return catalog_ != nullptr; }
    bool status(EdfReportCatalogStatus &out, uint32_t timeout_ms) const;
    bool request_refresh() const;
    bool pending_or_request_refresh() const;
    bool timezone_offset_minutes(int32_t &out) const;
    size_t session_count() const;
    bool copy_session(size_t index, EdfReportSessionDescriptor &out) const;
    bool session_reportable(
        const EdfReportSessionDescriptor &session) const;
    bool resolve_session_timezone(
        EdfReportSessionDescriptor &session,
        const ReportSummaryRecord *matching_summary,
        int32_t &offset_minutes) const;
    bool collect_sessions_for_night(const ReportSummaryRecord &night,
                                    int64_t range_start_ms,
                                    int64_t range_end_ms,
                                    EdfReportSessionDescriptor *sessions,
                                    size_t session_capacity,
                                    size_t &session_count,
                                    bool *pending_out = nullptr) const;

    bool ready_refresh_changed(EdfReportCatalogStatus &out) const;
    void mark_summary_published(uint32_t refresh_id);

private:
    EdfReportCatalogJob *catalog_ = nullptr;
    uint32_t summary_refresh_id_ = 0;
};

}  // namespace aircannect
