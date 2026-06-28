#include "report_source_resolver.h"

#include <algorithm>
#include <string.h>

namespace aircannect {
namespace {

constexpr int64_t COVERAGE_TOLERANCE_MS = 5 * 60 * 1000;

bool report_ranges_overlap_local(int64_t a_start,
                                 int64_t a_end,
                                 int64_t b_start,
                                 int64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

size_t collect_required_ranges(const ReportSessionRange *session_ranges,
                               size_t session_range_count,
                               int64_t range_start_ms,
                               int64_t range_end_ms,
                               ReportSessionRange *required_ranges,
                               size_t max_ranges) {
    if (!session_ranges || !required_ranges || max_ranges == 0 ||
        range_end_ms <= range_start_ms) {
        return 0;
    }
    size_t required_range_count = 0;
    for (size_t i = 0; i < session_range_count &&
                       required_range_count < max_ranges; ++i) {
        if (!report_ranges_overlap_local(session_ranges[i].start_ms,
                                         session_ranges[i].end_ms,
                                         range_start_ms,
                                         range_end_ms)) {
            continue;
        }
        ReportSessionRange &range = required_ranges[required_range_count];
        range.start_ms = std::max(session_ranges[i].start_ms, range_start_ms);
        range.end_ms = std::min(session_ranges[i].end_ms, range_end_ms);
        if (range.end_ms > range.start_ms) required_range_count++;
    }
    return required_range_count;
}

struct CoverageCollectContext {
    ReportCoverageInterval *intervals = nullptr;
    size_t max_intervals = 0;
    size_t interval_count = 0;
};

bool remember_provider_interval(void *context,
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

bool merge_intervals(ReportCoverageInterval *intervals,
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

bool gap_exceeds_edf_tolerance(int64_t start_ms, int64_t end_ms) {
    return end_ms > start_ms &&
           end_ms - start_ms > AC_EDF_REPORT_COVERAGE_TOLERANCE_MS;
}

const ReportResolvedStream *find_stream(const ReportResolvedPlan &plan,
                                        ReportStoreChunkKind kind,
                                        const char *name) {
    if (!name || !name[0]) return nullptr;
    for (size_t i = 0; i < plan.stream_count; ++i) {
        const ReportResolvedStream &stream = plan.streams[i];
        if (stream.kind == kind && stream.name &&
            strcmp(stream.name, name) == 0) {
            return &stream;
        }
    }
    return nullptr;
}

}  // namespace

ReportSourceResolver::ReportSourceResolver(
    const ReportDataProvider &edf_provider,
    const ReportDataProvider &spool_provider,
    ReportResolveScratch &scratch)
    : edf_(edf_provider), spool_(spool_provider), scratch_(scratch) {}

bool ReportSourceResolver::build_plan(const ReportIndexedNight &night,
                                      int64_t range_start_ms,
                                      int64_t range_end_ms,
                                      ReportResolvedPlan &out) const {
    // ReportResolvedPlan is intentionally a large PSRAM-backed work buffer.
    // Value-assigning a default instance can materialize the whole object on
    // the main-loop stack, which is exactly where report builds must not spend
    // several kilobytes.
    memset(&out, 0, sizeof(out));
    if (range_end_ms <= range_start_ms) return true;

    ReportSessionRange *source_ranges = scratch_.source_ranges;
    const size_t source_range_count =
        collect_indexed_night_data_ranges(night,
                                          source_ranges,
                                          AC_REPORT_SUMMARY_SESSION_MAX);

    ReportSessionRange *required_ranges = scratch_.required_ranges;
    const size_t required_range_count =
        collect_required_ranges(source_ranges,
                                source_range_count,
                                range_start_ms,
                                range_end_ms,
                                required_ranges,
                                AC_REPORT_SUMMARY_SESSION_MAX);
    if (required_range_count == 0) return true;

    out.range_count = required_range_count;
    for (size_t i = 0; i < required_range_count; ++i) {
        out.ranges[i] = required_ranges[i];
    }

    if (!add_events(required_ranges, required_range_count, out)) {
        return false;
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t i = 0; i < signal_count; ++i) {
        if (!add_signal(night,
                        signals[i],
                        required_ranges,
                        required_range_count,
                        out)) {
            return false;
        }
    }

    out.missing_required = 0;
    for (size_t i = 0; i < out.stream_count; ++i) {
        if (out.streams[i].required && !out.streams[i].complete) {
            out.missing_required++;
        }
    }
    return true;
}

bool ReportSourceResolver::add_stream(ReportResolvedPlan &plan,
                                      ReportStoreChunkKind kind,
                                      ReportSignalId signal,
                                      const char *name,
                                      ReportSourceId preferred_source,
                                      ReportSourceId selected_source,
                                      ReportResolvedProvider provider,
                                      bool required,
                                      bool complete,
                                      size_t &stream_index) const {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < plan.stream_count; ++i) {
        ReportResolvedStream &stream = plan.streams[i];
        if (stream.kind == kind &&
            stream.signal == signal &&
            stream.name &&
            strcmp(stream.name, name) == 0) {
            stream_index = i;
            if (required) stream.required = true;
            stream.has_covered_segment =
                stream.has_covered_segment ||
                (provider != ReportResolvedProvider::None && complete);
            stream.has_missing_segment =
                stream.has_missing_segment || !complete;
            stream.has_edf_segment =
                stream.has_edf_segment ||
                provider == ReportResolvedProvider::Edf;
            stream.has_spool_segment =
                stream.has_spool_segment ||
                provider == ReportResolvedProvider::Spool;
            stream.complete = !stream.has_missing_segment;
            if (provider == ReportResolvedProvider::Edf ||
                (stream.provider == ReportResolvedProvider::None &&
                 provider != ReportResolvedProvider::None) ||
                (!complete && !stream.has_covered_segment)) {
                stream.selected_source = selected_source;
                stream.provider = provider;
            }
            if (selected_source != preferred_source) stream.low_res = true;
            return true;
        }
    }
    if (plan.stream_count >= AC_REPORT_RESOLVED_STREAM_MAX) return false;
    stream_index = plan.stream_count++;
    ReportResolvedStream &stream = plan.streams[stream_index];
    stream.kind = kind;
    stream.signal = signal;
    stream.name = name;
    stream.preferred_source = preferred_source;
    stream.selected_source = selected_source;
    stream.provider = provider;
    stream.required = required;
    stream.complete = complete;
    stream.low_res = selected_source != preferred_source;
    stream.has_covered_segment =
        provider != ReportResolvedProvider::None && complete;
    stream.has_missing_segment = !complete;
    stream.has_edf_segment = provider == ReportResolvedProvider::Edf;
    stream.has_spool_segment = provider == ReportResolvedProvider::Spool;
    return true;
}

bool ReportSourceResolver::add_segment(ReportResolvedPlan &plan,
                                       size_t stream_index,
                                       ReportStoreChunkKind kind,
                                       ReportSignalId signal,
                                       const char *name,
                                       ReportSourceId source,
                                       ReportResolvedProvider provider,
                                       int64_t start_ms,
                                       int64_t end_ms,
                                       bool required,
                                       bool complete) const {
    if (stream_index >= plan.stream_count || end_ms <= start_ms) return true;
    if (plan.segment_count >= AC_REPORT_RESOLVED_SEGMENT_MAX) return false;
    ReportResolvedSegment &segment = plan.segments[plan.segment_count++];
    segment.stream_index = stream_index;
    segment.kind = kind;
    segment.signal = signal;
    segment.name = name;
    segment.source = source;
    segment.provider = provider;
    segment.start_ms = start_ms;
    segment.end_ms = end_ms;
    segment.required = required;
    segment.complete = complete;
    return true;
}

bool ReportSourceResolver::source_chunk_extent(
    const ReportIndexedNight &night,
    ReportSourceId source,
    const char *name,
    int64_t &min_start,
    int64_t &max_end) const {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0] || !name || !name[0]) {
        return false;
    }
    int64_t span_start = 0;
    int64_t span_end = 0;
    if (!indexed_night_data_span(night, span_start, span_end)) return false;
    ReportProviderChunkExtent extent;
    if (!spool_.chunk_extent(ReportStoreChunkKind::Series,
                             *def,
                             name,
                             static_cast<int64_t>(night.summary.start_ms),
                             span_start,
                             span_end,
                             extent)) {
        return false;
    }
    min_start = extent.min_start_ms;
    max_end = extent.max_end_ms;
    return true;
}

ReportSourceId ReportSourceResolver::choose_spool_source_for_signal(
    const ReportIndexedNight &night,
    const ReportSignalDef &signal) const {
    ReportSourceId source = signal.preferred_source;
    if (source == signal.fallback_source) return source;

    int64_t p_min = 0, p_max = 0, f_min = 0, f_max = 0;
    const bool p_has = source_chunk_extent(night,
                                           source,
                                           signal.store_name,
                                           p_min,
                                           p_max);
    const bool f_has = source_chunk_extent(night,
                                           signal.fallback_source,
                                           signal.store_name,
                                           f_min,
                                           f_max);
    if (!p_has || (f_has && (p_min > f_min + COVERAGE_TOLERANCE_MS ||
                             p_max < f_max - COVERAGE_TOLERANCE_MS))) {
        source = signal.fallback_source;
    }
    return source;
}

bool ReportSourceResolver::add_events(const ReportSessionRange *ranges,
                                      size_t range_count,
                                      ReportResolvedPlan &plan) const {
    if (!ranges || range_count == 0) return true;

    const ReportSourceDef *source =
        report_source_def(ReportSourceId::RespiratoryEvents);
    if (!source) return false;
    const char *event_name = report_source_spool_type(source->id);
    bool have_event_stream = false;
    size_t event_stream_index = 0;

    auto add_event_segment = [&](ReportResolvedProvider provider,
                                 int64_t start_ms,
                                 int64_t end_ms,
                                 bool complete) -> bool {
        if (!add_stream(plan,
                        ReportStoreChunkKind::Events,
                        ReportSignalId::Flow,
                        event_name,
                        source->id,
                        source->id,
                        provider,
                        false,
                        complete,
                        event_stream_index)) {
            return false;
        }
        have_event_stream = true;
        return add_segment(plan,
                           event_stream_index,
                           ReportStoreChunkKind::Events,
                           ReportSignalId::Flow,
                           event_name,
                           source->id,
                           provider,
                           start_ms,
                           end_ms,
                           false,
                           complete);
    };

    auto collect = [&](const ReportDataProvider &provider,
                       ReportResolvedProvider provider_id,
                       const char *name,
                       int64_t start_ms,
                       int64_t end_ms,
                       size_t &interval_count) -> bool {
        CoverageCollectContext ctx;
        ctx.intervals = scratch_.coverage;
        ctx.max_intervals = AC_REPORT_RESOLVED_SEGMENT_MAX;
        if (!provider.for_each_chunk(ReportStoreChunkKind::Events,
                                     *source,
                                     ReportSignalId::Flow,
                                     name,
                                     0,
                                     start_ms,
                                     end_ms,
                                     remember_provider_interval,
                                     &ctx)) {
            return false;
        }
        for (size_t i = 0; i < ctx.interval_count; ++i) {
            scratch_.coverage[i].source = source->id;
        }
        interval_count = ctx.interval_count;
        (void)provider_id;
        return merge_intervals(scratch_.coverage,
                               interval_count,
                               start_ms,
                               end_ms);
    };

    for (size_t range_index = 0; range_index < range_count; ++range_index) {
        const ReportSessionRange &range = ranges[range_index];
        if (range.end_ms <= range.start_ms) continue;

        size_t edf_count = 0;
        if (!collect(edf_,
                     ReportResolvedProvider::Edf,
                     event_name,
                     range.start_ms,
                     range.end_ms,
                     edf_count)) {
            return false;
        }

        int64_t cursor = range.start_ms;
        for (size_t i = 0; i < edf_count; ++i) {
            const ReportCoverageInterval &interval = scratch_.coverage[i];
            if (interval.end_ms <= cursor) continue;
            if (interval.start_ms > cursor) {
                const bool complete = spool_.coverage_complete(*source,
                                                               cursor,
                                                               interval.start_ms);
                if (!add_event_segment(ReportResolvedProvider::Spool,
                                       cursor,
                                       interval.start_ms,
                                       complete)) {
                    return false;
                }
            }
            if (!add_event_segment(ReportResolvedProvider::Edf,
                                   std::max(interval.start_ms, range.start_ms),
                                   std::min(interval.end_ms, range.end_ms),
                                   true)) {
                return false;
            }
            cursor = std::max(cursor, interval.end_ms);
        }

        if (cursor < range.end_ms) {
            const bool complete = spool_.coverage_complete(*source,
                                                           cursor,
                                                           range.end_ms);
            if (!add_event_segment(ReportResolvedProvider::Spool,
                                   cursor,
                                   range.end_ms,
                                   complete)) {
                return false;
            }
        }

        size_t csr_count = 0;
        if (!collect(edf_,
                     ReportResolvedProvider::Edf,
                     "TherapyEvents-CSR",
                     range.start_ms,
                     range.end_ms,
                     csr_count)) {
            return false;
        }
        for (size_t i = 0; i < csr_count; ++i) {
            size_t csr_stream_index = 0;
            if (!add_stream(plan,
                            ReportStoreChunkKind::Events,
                            ReportSignalId::Flow,
                            "TherapyEvents-CSR",
                            source->id,
                            source->id,
                            ReportResolvedProvider::Edf,
                            false,
                            true,
                            csr_stream_index)) {
                return false;
            }
            if (!add_segment(plan,
                             csr_stream_index,
                             ReportStoreChunkKind::Events,
                             ReportSignalId::Flow,
                             "TherapyEvents-CSR",
                             source->id,
                             ReportResolvedProvider::Edf,
                             scratch_.coverage[i].start_ms,
                             scratch_.coverage[i].end_ms,
                             false,
                             true)) {
                return false;
            }
        }
    }

    const ReportResolvedStream *stream =
        have_event_stream
            ? find_stream(plan, ReportStoreChunkKind::Events, event_name)
            : nullptr;
    plan.events_available =
        stream && stream->complete && stream->has_covered_segment;
    return true;
}

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

    const ReportSourceId spool_source = choose_spool_source_for_signal(night,
                                                                       signal);
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

    auto add_spool_gap = [&](int64_t start_ms,
                             int64_t end_ms) -> bool {
        if (end_ms <= start_ms) return true;
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
