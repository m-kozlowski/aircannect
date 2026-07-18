#include "report_result_prepare_service.h"

#include <string.h>

#include "debug_log.h"
#include "report_index_scratch.h"

namespace aircannect {

ReportResultPrepareService::ReportResultPrepareService(
    ReportResultBuildService &result_build,
    ReportResultCacheRuntime &result_cache,
    ReportNightIndexService &night_index,
    ReportCacheFetchService &cache_fetch)
    : result_build_(result_build),
      result_cache_(result_cache),
      night_index_(night_index),
      cache_fetch_(cache_fetch) {}

void ReportResultPrepareService::clear_prepare() {
    result_build_.clear_prepare();
}

void ReportResultPrepareService::fail_prepare(const char *message) {
    result_build_.fail_prepare(message);
}

bool ReportResultPrepareService::refresh_cache_if_needed(
    const ReportIndexedNight &night,
    size_t therapy_index,
    bool refresh_cache,
    bool &deferred) {
    deferred = false;
    if (!refresh_cache || runtime().status().missing_required == 0) return true;

    const bool latest_tail_refresh = therapy_index == 0;
    if (!cache_fetch_.active()) {
        if (!cache_fetch_.build_plan(night, false, latest_tail_refresh)) {
            return false;
        }

        if (cache_fetch_.has_sources()) {
            result_cache_.invalidate(night.summary.start_ms, false);

            const ReportCacheFetchEvent event = cache_fetch_.start_next_source();
            if (event == ReportCacheFetchEvent::Failed) {
                fail_prepare(cache_fetch_.status().error.c_str());
                return false;
            }
        }
    }

    const bool cache_refresh_in_flight =
        cache_fetch_.active() &&
        cache_fetch_.state().night().start_ms == night.summary.start_ms;
    if (cache_refresh_in_flight) {
        cache_fetch_.set_pending_prepare(therapy_index, false);
        runtime().status().state = ReportResultState::Preparing;
        runtime().status().error = "cache_fetching";
        deferred = true;
    }
    return true;
}

ReportResultPrepareService::ResultPrepareOutcome
ReportResultPrepareService::prepare_by_therapy_index(
    size_t therapy_index,
    bool refresh_cache) {
    if (!result_build_.ensure_chunks()) return ResultPrepareOutcome::Failed;

    ScopedIndexedNight indexed_night("prepare_result_night_index");
    if (!indexed_night) return ResultPrepareOutcome::Retry;

    const ReportNightIndexLookupResult lookup =
        night_index_.by_therapy_index(therapy_index, indexed_night.get());
    if (lookup == ReportNightIndexLookupResult::Busy) {
        result_build_.defer_prepare(0, therapy_index, "night_index_busy");
        return ResultPrepareOutcome::Deferred;
    }
    if (lookup == ReportNightIndexLookupResult::Failed) {
        result_build_.defer_prepare(0, therapy_index, "night_index_failed");
        return ResultPrepareOutcome::Retry;
    }
    if (lookup == ReportNightIndexLookupResult::NotFound) {
        clear_prepare();
        fail_prepare("night_not_found");
        return ResultPrepareOutcome::Failed;
    }

    return prepare_by_night_start(
        indexed_night->summary.start_ms,
        therapy_index,
        refresh_cache);
}

ReportResultPrepareService::ResultPrepareOutcome
ReportResultPrepareService::prepare_by_night_start(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh_cache) {
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Result prepare start night=%llu index=%lu refresh=%u\n",
              static_cast<unsigned long long>(night_start_ms),
              static_cast<unsigned long>(therapy_index),
              refresh_cache ? 1u : 0u);
    if (!result_build_.ensure_chunks()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result prepare failed before night load reason=chunk_alloc "
                  "night=%llu index=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index));
        return ResultPrepareOutcome::Failed;
    }

    if (!runtime().scratch().ensure_prepare_indexed_night()) {
        return ResultPrepareOutcome::Retry;
    }

    size_t current_therapy_index = therapy_index;
    ReportIndexedNight *prepare_indexed_night =
        runtime().scratch().prepare_indexed_night();
    memset(prepare_indexed_night, 0, sizeof(*prepare_indexed_night));

    const ReportNightIndexLookupResult lookup =
        night_index_.by_start(night_start_ms,
                              *prepare_indexed_night,
                              &current_therapy_index);
    if (lookup == ReportNightIndexLookupResult::Busy) {
        result_build_.defer_prepare(night_start_ms,
                                    therapy_index,
                                    "night_index_busy");
        return ResultPrepareOutcome::Deferred;
    }
    if (lookup == ReportNightIndexLookupResult::Failed) {
        result_build_.defer_prepare(night_start_ms,
                                    therapy_index,
                                    "night_index_failed");
        return ResultPrepareOutcome::Retry;
    }
    if (lookup == ReportNightIndexLookupResult::NotFound) {
        clear_prepare();
        fail_prepare("night_not_found");
        return ResultPrepareOutcome::Failed;
    }
    const ReportIndexedNight &indexed_night = *prepare_indexed_night;
    therapy_index = current_therapy_index;
    const ReportSummaryRecord &night = indexed_night.summary;
    if (indexed_night.edf_catalog_pending) {
        runtime().status().state = ReportResultState::Preparing;
        runtime().status().therapy_index = therapy_index;
        runtime().status().night_start_ms = night.start_ms;
        runtime().status().night_end_ms = night.end_ms;
        runtime().status().duration_min = night.duration_min;
        runtime().status().error = "edf_catalog_refreshing";
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result prepare deferred for EDF catalog "
                  "night=%llu index=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index));
        return ResultPrepareOutcome::Deferred;
    }

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    night_index_.format_result_etag(indexed_night,
                                    current_etag,
                                    sizeof(current_etag));
    if (!current_etag[0]) {
        runtime().status().error = "empty_etag";
        return ResultPrepareOutcome::Retry;
    }

    if (runtime().current_result_publishable(therapy_index,
                                             indexed_night,
                                             current_etag,
                                             refresh_cache) &&
        result_cache_.publish_result(runtime())) {
        return ResultPrepareOutcome::Prepared;
    }

    result_build_.begin_prepare_for_night(therapy_index,
                                          indexed_night,
                                          current_etag);
    runtime().plot().skip_cache = refresh_cache;

    // Use the session data span, not night.end_ms (a 24h day bucket far past the
    // therapy data) coverage is only written/checked over the session span,
    // so the result chunk range and its coverage check must match.
    ReportSessionRange night_range;
    if (!runtime().data_span(night_range.start_ms, night_range.end_ms)) {
        runtime().status().state = ReportResultState::Incomplete;
        runtime().status().error = "no_sessions";
        return result_cache_.publish_result(runtime())
                   ? ResultPrepareOutcome::Prepared
                   : ResultPrepareOutcome::Retry;
    }

    bool deferred = false;
    if (!result_build_.resolve_and_materialize_for_night(indexed_night,
                                                         night_range.start_ms,
                                                         night_range.end_ms,
                                                         &deferred)) {
        runtime().release_edf_sessions();
        return runtime().status().state == ReportResultState::Error
                   ? ResultPrepareOutcome::Failed
                   : ResultPrepareOutcome::Retry;
    }
    if (deferred) return ResultPrepareOutcome::Deferred;
    const bool uses_edf = runtime().uses_edf_provider();
    if (!uses_edf) {
        runtime().release_edf_sessions();
    }

    deferred = false;
    if (!refresh_cache_if_needed(indexed_night,
                                 therapy_index,
                                 refresh_cache,
                                 deferred)) {
        runtime().release_edf_sessions();
        return runtime().status().state == ReportResultState::Error
                   ? ResultPrepareOutcome::Failed
                   : ResultPrepareOutcome::Retry;
    }
    if (deferred) {
        runtime().release_edf_sessions();
        return ResultPrepareOutcome::Deferred;
    }

    const bool ok = result_build_.finalize_prepare(therapy_index);
    if (!result_build_.plot_builder().active()) {
        runtime().release_edf_sessions();
    }
    if (ok) return ResultPrepareOutcome::Prepared;
    return runtime().status().state == ReportResultState::Error
               ? ResultPrepareOutcome::Failed
               : ResultPrepareOutcome::Retry;
}

}  // namespace aircannect
