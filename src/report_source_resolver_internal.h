#pragma once

#include "report_source_resolver.h"

#include <algorithm>

namespace aircannect {
namespace report_source_resolver_detail {

struct CoverageCollectContext {
    ReportCoverageInterval *intervals = nullptr;
    size_t max_intervals = 0;
    size_t interval_count = 0;
};

inline bool remember_provider_interval(void *context,
                                       const ReportProviderChunk &chunk) {
    auto *ctx = static_cast<CoverageCollectContext *>(context);
    if (!ctx || !ctx->intervals || ctx->max_intervals == 0 ||
        chunk.end_ms <= chunk.start_ms) {
        return false;
    }
    if (ctx->interval_count >= ctx->max_intervals) return false;

    ReportCoverageInterval &interval = ctx->intervals[ctx->interval_count++];
    interval.start_ms = chunk.start_ms;
    interval.end_ms = chunk.end_ms;
    interval.source = chunk.source;
    return true;
}

inline bool merge_intervals(ReportCoverageInterval *intervals,
                            size_t &count,
                            int64_t range_start_ms,
                            int64_t range_end_ms) {
    if (!intervals) return false;
    if (count == 0) return true;

    std::sort(intervals,
              intervals + count,
              [](const ReportCoverageInterval &a,
                 const ReportCoverageInterval &b) {
                  if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
                  if (a.end_ms != b.end_ms) return a.end_ms < b.end_ms;
                  return static_cast<uint8_t>(a.source) <
                         static_cast<uint8_t>(b.source);
              });

    size_t merged_count = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t start_ms = std::max(intervals[i].start_ms, range_start_ms);
        int64_t end_ms = std::min(intervals[i].end_ms, range_end_ms);
        if (end_ms <= start_ms) continue;

        if (merged_count > 0 &&
            intervals[merged_count - 1].source == intervals[i].source &&
            start_ms <= intervals[merged_count - 1].end_ms) {
            if (end_ms > intervals[merged_count - 1].end_ms) {
                intervals[merged_count - 1].end_ms = end_ms;
            }
            continue;
        }

        intervals[merged_count++] = {start_ms, end_ms, intervals[i].source};
    }

    count = merged_count;
    return true;
}

inline bool gap_exceeds_edf_tolerance(int64_t start_ms, int64_t end_ms) {
    return end_ms > start_ms &&
           end_ms - start_ms > AC_EDF_REPORT_COVERAGE_TOLERANCE_MS;
}

}  // namespace report_source_resolver_detail
}  // namespace aircannect
