#include "report_result_range_set.h"

#include <algorithm>
#include <string.h>

namespace aircannect {

void ReportResultRangeSet::clear() {
    memset(ranges_, 0, sizeof(ranges_));
    count_ = 0;
}

bool ReportResultRangeSet::append(int64_t start_ms, int64_t end_ms) {
    if (count_ >= AC_REPORT_SUMMARY_SESSION_MAX) return false;
    if (start_ms <= 0 || end_ms <= start_ms) return false;

    PlotRange &range = ranges_[count_++];
    range.start_ms = start_ms;
    range.end_ms = end_ms;
    return true;
}

void ReportResultRangeSet::sort() {
    std::sort(ranges_,
              ranges_ + count_,
              [](const PlotRange &a, const PlotRange &b) {
                  return a.start_ms < b.start_ms;
              });
}

bool ReportResultRangeSet::set_from_indexed_night(
    const ReportIndexedNight &night) {
    clear();

    ReportSessionRange ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    const size_t range_count =
        collect_indexed_night_report_ranges(night,
                                            ranges,
                                            AC_REPORT_SUMMARY_SESSION_MAX);
    for (size_t i = 0; i < range_count; ++i) {
        (void)append(ranges[i].start_ms, ranges[i].end_ms);
    }

    sort();
    return count_ > 0;
}

bool ReportResultRangeSet::set_from_edf_sessions(
    const EdfReportSessionDescriptor *sessions,
    size_t session_count) {
    if (!sessions || session_count == 0) return false;

    clear();
    for (size_t i = 0; i < session_count; ++i) {
        const EdfReportSessionDescriptor &session = sessions[i];
        if (!edf_session_has_report_numeric(session)) continue;

        (void)append(session.earliest_header_start_ms,
                     session.latest_header_end_ms);
    }

    sort();
    return count_ > 0;
}

bool ReportResultRangeSet::set_from_resolved_plan(
    const ReportResolvedPlan &plan) {
    clear();

    const size_t range_count =
        std::min(plan.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < range_count; ++i) {
        (void)append(plan.ranges[i].start_ms, plan.ranges[i].end_ms);
    }

    sort();
    return count_ > 0;
}

bool ReportResultRangeSet::data_span(const ReportIndexedNight &indexed_night,
                                     const ReportSummaryRecord &night,
                                     int64_t &span_start_ms,
                                     int64_t &span_end_ms) const {
    if (count_ == 0) {
        return indexed_night_data_span(indexed_night,
                                       span_start_ms,
                                       span_end_ms) ||
               night_data_span(night, span_start_ms, span_end_ms);
    }

    span_start_ms = ranges_[0].start_ms;
    span_end_ms = ranges_[0].end_ms;
    for (size_t i = 1; i < count_; ++i) {
        span_start_ms = std::min(span_start_ms, ranges_[i].start_ms);
        span_end_ms = std::max(span_end_ms, ranges_[i].end_ms);
    }
    return span_end_ms > span_start_ms;
}

bool ReportResultRangeSet::contains_timestamp(int64_t timestamp_ms) const {
    if (count_ == 0) return true;

    for (size_t i = 0; i < count_; ++i) {
        const PlotRange &range = ranges_[i];
        if (timestamp_ms >= range.start_ms && timestamp_ms <= range.end_ms) {
            return true;
        }
    }

    return false;
}

}  // namespace aircannect
