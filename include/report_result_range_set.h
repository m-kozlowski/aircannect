#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_manager_internal_types.h"
#include "report_manager_limits.h"
#include "report_night_index.h"
#include "report_source_resolver.h"

namespace aircannect {

class ReportResultRangeSet {
public:
    using PlotRange = report_manager_internal::PlotRange;

    void clear();
    bool append(int64_t start_ms, int64_t end_ms);
    void sort();

    bool set_from_indexed_night(const ReportIndexedNight &night);
    bool set_from_edf_sessions(const EdfReportSessionDescriptor *sessions,
                               size_t session_count);
    bool set_from_resolved_plan(const ReportResolvedPlan &plan);

    bool data_span(const ReportIndexedNight &indexed_night,
                   const ReportSummaryRecord &night,
                   int64_t &span_start_ms,
                   int64_t &span_end_ms) const;
    bool contains_timestamp(int64_t timestamp_ms) const;

    const PlotRange *data() const { return ranges_; }
    PlotRange *data() { return ranges_; }
    size_t count() const { return count_; }

    const PlotRange &operator[](size_t index) const { return ranges_[index]; }
    PlotRange &operator[](size_t index) { return ranges_[index]; }

private:
    PlotRange ranges_[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    size_t count_ = 0;
};

}  // namespace aircannect
