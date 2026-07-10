#pragma once

#include "report_night_index.h"

namespace aircannect {

constexpr int64_t REPORT_SESSION_MERGE_TOLERANCE_MS = 2 * 60 * 1000;
constexpr int64_t REPORT_DAY_MS = 24LL * 60LL * 60LL * 1000LL;
constexpr int64_t REPORT_NOON_MS = 12LL * 60LL * 60LL * 1000LL;

void normalize_range_array(ReportSessionRange *ranges, size_t &count);
void coalesce_sorted_range_array(ReportSessionRange *ranges, size_t &count);
uint64_t report_edf_session_signature(
    const EdfReportSessionDescriptor &session);
void recompute_indexed_night_source_signature(ReportIndexedNight &night);

}  // namespace aircannect
