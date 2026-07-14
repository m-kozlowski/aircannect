#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include "edf_report_provider.h"
#include "memory_manager.h"
#include "report_cache_plan.h"
#include "report_data_provider.h"
#include "report_diagnostics.h"
#include "report_index_scratch.h"
#include "report_night_cache_service.h"
#include "report_resolve_context.h"
#include "report_source_resolver.h"
#include "report_store.h"
#include "report_summary_json.h"

namespace aircannect {
namespace {

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

void invalidate_active_fetch_result(ReportCacheFetchService &cache_fetch,
                                    ReportResultCacheRuntime &result_cache) {
    if (!cache_fetch.has_sources()) return;

    result_cache.invalidate(cache_fetch.state().night().start_ms, false);
}

}  // namespace

ReportNightCacheService::ReportNightCacheService(
    ReportNightIndexService &night_index,
    ReportEdfCatalogContext &edf_catalog,
    ReportCacheFetchService &cache_fetch)
    : night_index_(night_index),
      edf_catalog_(edf_catalog),
      cache_fetch_(cache_fetch) {}

bool ReportNightCacheService::coverage(
    uint64_t night_start_ms,
    ReportNightCoverageStatus &out) const {
    out = {};
    ScopedIndexedNight indexed_night("night_coverage_index");
    if (!indexed_night ||
        !night_index_.by_start(night_start_ms, indexed_night.get())) {
        return false;
    }
    const ReportIndexedNight &indexed = indexed_night.get();
    const ReportSummaryRecord &night = indexed.summary;

    out.found = true;
    out.start_ms = night.start_ms;
    out.end_ms = night.end_ms;
    out.duration_min = report_indexed_night_display_duration_min(indexed);

    int64_t span_start_ms = 0;
    int64_t span_end_ms = 0;
    if (!indexed_night_data_span(indexed, span_start_ms, span_end_ms)) {
        return true;
    }

    ScopedReportResolveContext resolve("night_coverage_resolver");
    if (!resolve) {
        return false;
    }

    bool pending = false;
    size_t session_count = 0;
    if (!edf_catalog_.collect_sessions_for_night(night,
                                                 span_start_ms,
                                                 span_end_ms,
                                                 resolve.sessions(),
                                                 AC_REPORT_EDF_SESSION_MAX,
                                                 session_count,
                                                 &pending) ||
        pending) {
        return false;
    }

    EdfReportDataProvider edf_provider(resolve.sessions(), session_count);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  resolve.scratch());
    if (!resolver.build_plan(indexed,
                             span_start_ms,
                             span_end_ms,
                             resolve.plan())) {
        return false;
    }

    const ReportResolvedPlan &plan = resolve.plan();
    out.missing_required = plan.missing_required;
    for (size_t i = 0; i < plan.stream_count; ++i) {
        const ReportResolvedStream &stream = plan.streams[i];
        if (!report_cache_source_supported(stream.selected_source)) continue;
        ReportNightSourceCoverage *entry = nullptr;
        for (size_t existing = 0; existing < out.source_count; ++existing) {
            if (out.sources[existing].source == stream.selected_source) {
                entry = &out.sources[existing];
                break;
            }
        }
        if (!entry) {
            if (out.source_count >= AC_REPORT_NIGHT_SOURCE_MAX) break;
            entry = &out.sources[out.source_count++];
            entry->source = stream.selected_source;
            entry->complete = true;
        }
        entry->required = entry->required || stream.required;
        if (stream.required && !stream.complete) entry->complete = false;
    }
    return true;
}

bool ReportNightCacheService::next_needing_cache(
    uint64_t &night_start_ms_out,
    ReportNightCacheSkipFn skip,
    const void *skip_context) const {
    const uint32_t now = millis();
    ReportIndexedNight *nights =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!nights) {
        log_report_alloc_failed(
            "prefetch_night_index",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }
    size_t count = 0;
    if (!night_index_.build(nights,
                            AC_REPORT_SUMMARY_RECORD_MAX,
                            count)) {
        Memory::free(nights);
        return false;
    }
    ScopedReportResolveContext resolve("prefetch_resolver");
    if (!resolve) {
        Memory::free(nights);
        return false;
    }

    // Oldest-first: the spool is open-ended (fromDateTime -> now), so fetching
    // the OLDEST night with a gap streams every source from there forward and
    // backfills all newer nights in a single sweep (deduped on write)
    for (size_t i = 0; i < count; ++i) {
        const ReportIndexedNight &indexed = nights[i];
        const ReportSummaryRecord &record = indexed.summary;
        if (!record.valid || !record.duration_min) continue;
        if (skip && skip(record.start_ms, now, skip_context)) continue;
        int64_t span_start_ms = 0;
        int64_t span_end_ms = 0;
        if (!indexed_night_data_span(indexed, span_start_ms, span_end_ms)) {
            continue;
        }

        bool edf_pending = false;
        size_t session_count = 0;
        memset(resolve.sessions(),
               0,
               AC_REPORT_EDF_SESSION_MAX *
                   sizeof(EdfReportSessionDescriptor));
        if (!edf_catalog_.collect_sessions_for_night(record,
                                                     span_start_ms,
                                                     span_end_ms,
                                                     resolve.sessions(),
                                                     AC_REPORT_EDF_SESSION_MAX,
                                                     session_count,
                                                     &edf_pending)) {
            Memory::free(nights);
            return false;
        }
        if (edf_pending) continue;

        EdfReportDataProvider edf_provider(resolve.sessions(), session_count);
        ReportSourceResolver resolver(edf_provider,
                                      spool_report_provider(),
                                      resolve.scratch());
        if (!resolver.build_plan(indexed,
                                 span_start_ms,
                                 span_end_ms,
                                 resolve.plan())) {
            Memory::free(nights);
            return false;
        }
        const ReportResolvedPlan &plan = resolve.plan();
        for (size_t segment_index = 0; segment_index < plan.segment_count;
             ++segment_index) {
            const ReportResolvedSegment &segment =
                plan.segments[segment_index];
            if (segment.provider == ReportResolvedProvider::Spool &&
                !segment.complete &&
                segment.required &&
                report_cache_source_supported(segment.source)) {
                night_start_ms_out = record.start_ms;
                Memory::free(nights);
                return true;
            }
        }
    }
    Memory::free(nights);
    return false;
}

bool ReportNightCacheService::request_cache(uint64_t night_start_ms,
                                            bool force) {
    if (cache_fetch_.active()) return false;

    ScopedIndexedNight indexed_night("request_night_cache_index");
    if (!indexed_night ||
        !night_index_.by_start(night_start_ms, indexed_night.get()) ||
        !report_indexed_night_visible_in_summary(indexed_night.get())) {
        return false;
    }

    if (!cache_fetch_.build_plan(indexed_night.get(), force, false)) {
        return false;
    }

    if (!cache_fetch_.has_sources()) {
        return cache_fetch_.finish() != ReportCacheFetchEvent::Failed;
    }

    const ReportCacheFetchEvent event = cache_fetch_.start_next_source();
    return event != ReportCacheFetchEvent::Failed;
}

bool ReportManager::night_etag(size_t therapy_index, char *out,
                               size_t out_size) const {
    return night_query_.night_etag(therapy_index, out, out_size);
}

bool ReportManager::for_each_summary_night(
    ReportSummaryNightCallback callback,
    void *context) const {
    return night_query_.for_each_summary_night(callback, context);
}

bool ReportManager::summary_night_by_therapy_index(
    size_t therapy_index,
    ReportSummaryRecord &out) const {
    return night_query_.summary_night_by_therapy_index(therapy_index, out);
}

bool ReportManager::latest_summary_night(ReportSummaryRecord &out) const {
    return night_query_.latest_summary_night(out);
}

bool ReportManager::night_coverage(uint64_t night_start_ms,
                                   ReportNightCoverageStatus &out) const {
    return night_cache_.coverage(night_start_ms, out);
}

bool ReportManager::request_night_cache(uint64_t night_start_ms, bool force) {
    if (summary_service_.active() || range_plot_builder_.active()) {
        return false;
    }
    if (!night_cache_.request_cache(night_start_ms, force)) return false;

    invalidate_active_fetch_result(cache_fetch_, result_cache_);
    return true;
}

}  // namespace aircannect
