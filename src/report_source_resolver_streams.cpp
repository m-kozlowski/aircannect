#include "report_source_resolver.h"

#include "report_source_resolver_internal.h"

#include <algorithm>

namespace aircannect {
namespace {

using report_source_resolver_detail::CoverageCollectContext;
using report_source_resolver_detail::gap_exceeds_edf_tolerance;
using report_source_resolver_detail::merge_intervals;
using report_source_resolver_detail::remember_provider_interval;

}  // namespace

bool ReportSourceResolver::add_signal(
    const ReportIndexedNight &night,
    const ReportSignalDef &signal,
    const ReportSessionRange *ranges,
    size_t range_count,
    ReportResolvedPlan &plan) const {
    const bool required = report_signal_required_for_result(signal);
    if (!ranges || range_count == 0 || !signal.store_name ||
        !signal.store_name[0]) {
        return true;
    }

    const bool spool_allowed = !night.has_edf_clock_provenance;
    const ReportSourceId spool_source = spool_allowed
        ? choose_spool_source_for_signal(night, signal)
        : signal.preferred_source;
    const ReportSourceDef *spool_source_def = report_source_def(spool_source);
    const ReportSourceDef *edf_source_def = report_source_def(
        signal.preferred_source);
    if (!edf_source_def) return false;

    auto collect = [&](const ReportDataProvider &provider,
                       ReportStoreChunkKind kind,
                       const ReportSourceDef &source,
                       ReportCoverageInterval *intervals,
                       int64_t start_ms,
                       int64_t end_ms,
                       size_t &interval_count) -> bool {
        CoverageCollectContext ctx;
        ctx.intervals = intervals;
        ctx.max_intervals = AC_REPORT_RESOLVED_SEGMENT_MAX;
        if (!provider.for_each_chunk(kind,
                                     source,
                                     signal.id,
                                     signal.store_name,
                                     static_cast<int64_t>(
                                         night.summary.start_ms),
                                     start_ms,
                                     end_ms,
                                     remember_provider_interval,
                                     &ctx)) {
            return false;
        }

        interval_count = ctx.interval_count;
        return merge_intervals(intervals,
                               interval_count,
                               start_ms,
                               end_ms);
    };

    auto add_series_segment = [&](ReportResolvedProvider provider,
                                  ReportSourceId source,
                                  int64_t start_ms,
                                  int64_t end_ms,
                                  bool complete) -> bool {
        if (end_ms <= start_ms) return true;
        if (!required && !complete) return true;

        size_t stream_index = 0;
        if (!add_stream(plan,
                        ReportStoreChunkKind::Series,
                        signal.id,
                        signal.store_name,
                        signal.preferred_source,
                        source,
                        provider,
                        required,
                        complete,
                        stream_index)) {
            return false;
        }

        return add_segment(plan,
                           stream_index,
                           ReportStoreChunkKind::Series,
                           signal.id,
                           signal.store_name,
                           source,
                           provider,
                           start_ms,
                           end_ms,
                           required,
                           complete);
    };

    auto add_spool_gap = [&](int64_t start_ms, int64_t end_ms) -> bool {
        if (end_ms <= start_ms) return true;
        if (!spool_allowed) {
            return add_series_segment(ReportResolvedProvider::None,
                                      signal.preferred_source,
                                      start_ms,
                                      end_ms,
                                      false);
        }
        if (!spool_source_def) {
            return add_series_segment(ReportResolvedProvider::Spool,
                                      spool_source,
                                      start_ms,
                                      end_ms,
                                      false);
        }

        const bool complete =
            spool_.coverage_complete(*spool_source_def, start_ms, end_ms);
        if (complete) {
            return add_series_segment(ReportResolvedProvider::Spool,
                                      spool_source,
                                      start_ms,
                                      end_ms,
                                      true);
        }

        size_t spool_interval_count = 0;
        if (!collect(spool_,
                     ReportStoreChunkKind::Series,
                     *spool_source_def,
                     scratch_.fallback,
                     start_ms,
                     end_ms,
                     spool_interval_count)) {
            return false;
        }
        for (size_t i = 0; i < spool_interval_count; ++i) {
            const ReportCoverageInterval &interval = scratch_.fallback[i];
            if (!add_series_segment(ReportResolvedProvider::Spool,
                                    interval.source,
                                    interval.start_ms,
                                    interval.end_ms,
                                    true)) {
                return false;
            }
        }
        if (required) {
            return add_series_segment(ReportResolvedProvider::Spool,
                                      spool_source,
                                      start_ms,
                                      end_ms,
                                      false);
        }

        return true;
    };

    auto add_required_spool_gap = [&](int64_t start_ms,
                                      int64_t end_ms) -> bool {
        if (!gap_exceeds_edf_tolerance(start_ms, end_ms)) return true;
        return add_spool_gap(start_ms, end_ms);
    };

    for (size_t range_index = 0; range_index < range_count; ++range_index) {
        const ReportSessionRange &range = ranges[range_index];
        if (range.end_ms <= range.start_ms) continue;

        size_t edf_interval_count = 0;
        if (!collect(edf_,
                     ReportStoreChunkKind::Series,
                     *edf_source_def,
                     scratch_.coverage,
                     range.start_ms,
                     range.end_ms,
                     edf_interval_count)) {
            return false;
        }
        if (edf_interval_count == 0) {
            if (!add_spool_gap(range.start_ms, range.end_ms)) return false;
            continue;
        }

        int64_t cursor = range.start_ms;
        for (size_t interval_index = 0; interval_index < edf_interval_count;
             ++interval_index) {
            const ReportCoverageInterval &interval =
                scratch_.coverage[interval_index];
            if (interval.end_ms <= cursor) continue;
            if (interval.start_ms > cursor) {
                if (!add_required_spool_gap(cursor, interval.start_ms)) {
                    return false;
                }
            }
            if (!add_series_segment(ReportResolvedProvider::Edf,
                                    interval.source,
                                    std::max(interval.start_ms,
                                             range.start_ms),
                                    std::min(interval.end_ms, range.end_ms),
                                    true)) {
                return false;
            }
            cursor = std::max(cursor, interval.end_ms);
        }

        if (range.end_ms > cursor) {
            if (!add_required_spool_gap(cursor, range.end_ms)) return false;
        }
    }

    return true;
}

}  // namespace aircannect
