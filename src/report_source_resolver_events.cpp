#include "report_source_resolver.h"

#include "report_source_resolver_internal.h"

#include <string.h>

namespace aircannect {
namespace {

using report_source_resolver_detail::CoverageCollectContext;
using report_source_resolver_detail::merge_intervals;
using report_source_resolver_detail::remember_provider_interval;

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

bool ReportSourceResolver::add_events(const ReportSessionRange *ranges,
                                      size_t range_count,
                                      bool spool_allowed,
                                      ReportResolvedPlan &plan) const {
    if (!ranges || range_count == 0) return true;

    const ReportSourceDef *source =
        report_source_def(ReportSourceId::RespiratoryEvents);
    if (!source) return false;

    const char *event_name = report_source_spool_type(source->id);
    bool have_event_stream = false;
    size_t event_stream_index = 0;
    bool saw_event_range = false;
    bool all_event_ranges_covered = true;

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
        return merge_intervals(scratch_.coverage,
                               interval_count,
                               start_ms,
                               end_ms);
    };

    auto add_collected_event_payloads = [&](ReportResolvedProvider provider_id,
                                            const char *name,
                                            int64_t start_ms,
                                            int64_t end_ms) -> bool {
        size_t interval_count = 0;
        const ReportDataProvider &provider =
            provider_id == ReportResolvedProvider::Edf ? edf_ : spool_;
        if (!collect(provider, name, start_ms, end_ms, interval_count)) {
            return false;
        }

        for (size_t i = 0; i < interval_count; ++i) {
            const ReportCoverageInterval &interval = scratch_.coverage[i];
            if (!add_event_segment(provider_id,
                                   interval.start_ms,
                                   interval.end_ms,
                                   true)) {
                return false;
            }
        }

        return true;
    };

    for (size_t range_index = 0; range_index < range_count; ++range_index) {
        const ReportSessionRange &range = ranges[range_index];
        if (range.end_ms <= range.start_ms) continue;

        saw_event_range = true;

        if (edf_.coverage_complete(*source, range.start_ms, range.end_ms)) {
            if (!add_event_segment(ReportResolvedProvider::Edf,
                                   range.start_ms,
                                   range.end_ms,
                                   true)) {
                return false;
            }
        } else if (spool_allowed &&
                   spool_.coverage_complete(*source,
                                            range.start_ms,
                                            range.end_ms)) {
            if (!add_event_segment(ReportResolvedProvider::Spool,
                                   range.start_ms,
                                   range.end_ms,
                                   true)) {
                return false;
            }
        } else {
            all_event_ranges_covered = false;
            if (!add_collected_event_payloads(ReportResolvedProvider::Edf,
                                              event_name,
                                              range.start_ms,
                                              range.end_ms)) {
                return false;
            }
            if (spool_allowed &&
                !add_collected_event_payloads(
                    ReportResolvedProvider::Spool,
                    event_name,
                    range.start_ms,
                    range.end_ms)) {
                return false;
            }
        }

        size_t csr_count = 0;
        if (!collect(edf_,
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
    plan.events_available = saw_event_range && all_event_ranges_covered &&
                            stream && stream->complete &&
                            stream->has_covered_segment;
    return true;
}

}  // namespace aircannect
