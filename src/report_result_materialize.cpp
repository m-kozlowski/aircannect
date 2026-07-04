#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include "debug_log.h"
#include "edf_report_provider.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_diagnostics.h"
#include "report_materializer.h"
#include "report_sources.h"
#include "report_source_resolver.h"
#include "report_store.h"
#include "report_summary_json.h"

namespace aircannect {
namespace {

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

}  // namespace

bool ReportManager::ensure_result_resolve_buffers() {
    if (!result_resolved_plan_) {
        result_resolved_plan_ = static_cast<ReportResolvedPlan *>(
            Memory::calloc_large(1, sizeof(ReportResolvedPlan), false));
        if (!result_resolved_plan_) {
            log_report_alloc_failed("result_resolved_plan",
                                    sizeof(ReportResolvedPlan));
            fail_result_prepare("result_plan_alloc_failed");
            return false;
        }
    }
    if (!result_resolve_scratch_) {
        result_resolve_scratch_ = static_cast<ReportResolveScratch *>(
            Memory::calloc_large(1, sizeof(ReportResolveScratch), false));
        if (!result_resolve_scratch_) {
            log_report_alloc_failed("result_resolve_scratch",
                                    sizeof(ReportResolveScratch));
            fail_result_prepare("result_scratch_alloc_failed");
            return false;
        }
    }
    return true;
}

bool ReportManager::source_chunk_extent(const ReportSummaryRecord &night,
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
    if (!night_data_span(night, span_start, span_end)) return false;
    ReportProviderChunkExtent extent;
    if (!spool_report_provider().chunk_extent(
            ReportStoreChunkKind::Series,
            *def,
            name,
            static_cast<int64_t>(night.start_ms),
            span_start,
            span_end,
            extent)) {
        return false;
    }
    min_start = extent.min_start_ms;
    max_end = extent.max_end_ms;
    return true;
}

bool ReportManager::begin_materialization(const ReportIndexedNight &night,
                                          const ReportResolvedPlan &plan) {
    clear_result_ranges();
    result_range_count_ =
        std::min(plan.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < result_range_count_; ++i) {
        result_ranges_[i].start_ms = plan.ranges[i].start_ms;
        result_ranges_[i].end_ms = plan.ranges[i].end_ms;
    }

    uint32_t duration_min = report_indexed_night_display_duration_min(night);
    if (duration_min == 0) {
        for (size_t i = 0; i < result_range_count_; ++i) {
            duration_min += report_ceil_duration_min(result_ranges_[i].start_ms,
                                                     result_ranges_[i].end_ms);
        }
    }
    if (duration_min > 0) {
        result_status_.duration_min = duration_min;
    } else if (night.has_summary && night.summary.duration_min > 0) {
        result_status_.duration_min = night.summary.duration_min;
    }
    return true;
}

bool ReportManager::add_materialized_stream(
    const ReportResolvedStream &stream,
    size_t &result_stream_index) {
    if (!add_result_stream(stream.kind,
                           stream.selected_source,
                           stream.signal,
                           stream.name,
                           stream.required,
                           stream.complete,
                           result_stream_index)) {
        return false;
    }
    if (result_stream_index < result_stream_count_) {
        ReportResultStream &result_stream =
            result_streams_[result_stream_index];
        result_stream.has_edf_segment =
            result_stream.has_edf_segment || stream.has_edf_segment;
        result_stream.has_spool_segment =
            result_stream.has_spool_segment || stream.has_spool_segment;
    }
    return true;
}

bool ReportManager::add_materialized_segment(
    const ReportResolvedSegment &segment,
    size_t result_stream_index) {
    if (segment.provider == ReportResolvedProvider::None) return true;

    if (segment.provider == ReportResolvedProvider::Edf) {
        EdfReportDataProvider provider(result_edf_sessions_,
                                       result_edf_session_count_);
        return add_provider_chunks_to_result_stream(
            provider,
            segment.kind,
            segment.source,
            segment.signal,
            segment.name,
            static_cast<int64_t>(result_night_.start_ms),
            segment.start_ms,
            segment.end_ms,
            segment.required,
            segment.complete,
            result_stream_index);
    }

    return add_provider_chunks_to_result_stream(
        spool_report_provider(),
        segment.kind,
        segment.source,
        segment.signal,
        segment.name,
        static_cast<int64_t>(result_night_.start_ms),
        segment.start_ms,
        segment.end_ms,
        segment.required,
        segment.complete,
        result_stream_index);
}

void ReportManager::finish_materialization(const ReportResolvedPlan &plan) {
    result_status_.events_available = plan.events_available;
    result_status_.missing_required = result_status_.missing_streams;
}

bool ReportManager::resolve_and_materialize_result_for_night(
    const ReportIndexedNight &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *edf_pending_out) {
    if (edf_pending_out) *edf_pending_out = false;
    bool edf_pending = false;
    bool have_edf =
        find_edf_sessions_for_night(night.summary,
                                    range_start_ms,
                                    range_end_ms,
                                    &edf_pending);
    if (edf_pending) {
        if (edf_pending_out) *edf_pending_out = true;
        result_status_.state = ReportResultState::Preparing;
        result_status_.error = "edf_catalog_refreshing";
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result waiting for EDF catalog "
                  "night=%llu from=%lld to=%lld\n",
                  static_cast<unsigned long long>(night.summary.start_ms),
                  static_cast<long long>(range_start_ms),
                  static_cast<long long>(range_end_ms));
        return true;
    }
    if (!ensure_result_resolve_buffers()) return false;

    EdfReportDataProvider edf_provider(have_edf ? result_edf_sessions_
                                                : nullptr,
                                       have_edf ? result_edf_session_count_
                                                : 0);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  *result_resolve_scratch_);
    ReportResolvedPlan &plan = *result_resolved_plan_;
    if (!resolver.build_plan(night, range_start_ms, range_end_ms, plan)) {
        fail_result_prepare("source_resolve_failed");
        return false;
    }

    ReportMaterializer materializer;
    if (!materializer.materialize(night, plan, *this)) {
        fail_result_prepare("source_materialize_failed");
        return false;
    }
    return true;
}

}  // namespace aircannect
