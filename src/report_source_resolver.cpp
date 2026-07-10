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
        collect_indexed_night_report_ranges(night,
                                            source_ranges,
                                            AC_REPORT_NIGHT_SESSION_MAX);

    ReportSessionRange *required_ranges = scratch_.required_ranges;
    const size_t required_range_count =
        collect_required_ranges(source_ranges,
                                source_range_count,
                                range_start_ms,
                                range_end_ms,
                                required_ranges,
                                AC_REPORT_NIGHT_SESSION_MAX);
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

}  // namespace aircannect
