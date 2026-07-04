#include "report_manager.h"

#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_daily_metrics.h"
#include "report_result_metrics.h"

namespace aircannect {
namespace {

constexpr uint32_t REPORT_STR_DURATION_TOLERANCE_MIN = 3;

}  // namespace

bool ReportManager::publish_existing_result_if_current(
    size_t therapy_index,
    const ReportIndexedNight &night,
    const char *current_etag,
    bool refresh_cache) {
    // Idempotent re-prepare: same indexed night and same Summary+EDF content
    // version already prepared with a plot -> keep it and republish. The ETag
    // check is required because another EDF session can be appended to the same
    // noon-noon night without changing the Summary start/end identity.
    if (!refresh_cache &&
        current_etag && current_etag[0] &&
        strcmp(result_etag_, current_etag) == 0 &&
        (result_status_.state == ReportResultState::Ready ||
         result_status_.state == ReportResultState::Partial) &&
        result_status_.therapy_index == therapy_index &&
        result_status_.chunk_count > 0 &&
        result_status_.night_start_ms == night.summary.start_ms &&
        result_plot_bin_.size() > 0) {
        return publish_result_to_slot();
    }
    return false;
}

void ReportManager::begin_result_prepare_for_night(
    size_t therapy_index,
    const ReportIndexedNight &night,
    const char *current_etag) {
    clear_result_prepare();
    result_status_.state = ReportResultState::Preparing;
    result_status_.therapy_index = therapy_index;

    result_indexed_night_ = night;
    result_night_ = night.summary;
    snprintf(result_etag_, sizeof(result_etag_), "%s",
             current_etag ? current_etag : "");
    set_result_ranges_from_indexed_night(night);
    result_status_.night_start_ms = night.summary.start_ms;
    result_status_.night_end_ms = night.summary.end_ms;
    result_status_.duration_min = night.summary.duration_min;

    ReportDailyMetrics metrics;
    if (report_daily_metrics_from_str_file(night.summary,
                                           REPORT_STR_DURATION_TOLERANCE_MIN,
                                           metrics)) {
        report_apply_daily_metrics_to_result_status(result_status_, metrics);
    }
    if (report_daily_metrics_from_summary(night.summary, metrics)) {
        report_apply_daily_metrics_to_result_status(result_status_, metrics);
    }
}

bool ReportManager::refresh_result_cache_if_needed(
    const ReportIndexedNight &night,
    size_t therapy_index,
    bool refresh_cache,
    bool &deferred) {
    deferred = false;
    if (!refresh_cache || result_status_.missing_required == 0) return true;

    const bool latest_tail_refresh = therapy_index == 0;
    prefetch_yield_to_foreground();
    if (!cache_fetch_active_) {
        if (!build_cache_plan(night, false, latest_tail_refresh)) {
            return false;
        }
        if (cache_source_count_ > 0) {
            if (!start_next_cache_source()) {
                return false;
            }
        } else {
            cache_fetch_active_ = false;
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error.clear();
        }
    }

    const bool cache_refresh_in_flight =
        cache_fetch_active_ &&
        cache_night_.start_ms == night.summary.start_ms;
    if (cache_refresh_in_flight) {
        pending_result_prepare_ = true;
        pending_result_refresh_cache_ = false;
        pending_result_therapy_index_ = therapy_index;
        result_status_.state = ReportResultState::Preparing;
        result_status_.error = "cache_fetching";
        deferred = true;
    }
    return true;
}

bool ReportManager::activate_cache_plan_for_night(
    const ReportSummaryRecord &night) {
    cache_status_.source_count = static_cast<uint32_t>(cache_source_count_);
    if (cache_source_count_ > 0) {
        invalidate_materialized(night.start_ms, false);
    }
    discard_cache_coalesce_buffers();
    begin_cache_write_fetch();
    cache_fetch_active_ = true;
    cache_status_.active = true;
    return true;
}

report_manager_internal::ResultPrepareOutcome
ReportManager::prepare_result_by_therapy_index_internal(
    size_t therapy_index,
    bool refresh_cache) {
    if (defer_result_prepare_for_summary(therapy_index, refresh_cache)) {
        return ResultPrepareOutcome::Deferred;
    }
    if (!ensure_result_chunks()) return ResultPrepareOutcome::Failed;

    ReportSummaryRecord night;
    if (!load_result_night(therapy_index, night)) {
        return ResultPrepareOutcome::Failed;
    }
    return prepare_result_by_night_start_internal(night.start_ms,
                                                 therapy_index,
                                                 refresh_cache);
}

report_manager_internal::ResultPrepareOutcome
ReportManager::prepare_result_by_night_start_internal(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh_cache) {
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Result prepare start night=%llu index=%lu refresh=%u\n",
              static_cast<unsigned long long>(night_start_ms),
              static_cast<unsigned long>(therapy_index),
              refresh_cache ? 1u : 0u);
    if (!ensure_result_chunks()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result prepare failed before night load reason=chunk_alloc "
                  "night=%llu index=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index));
        return ResultPrepareOutcome::Failed;
    }

    if (!prepare_indexed_night_) {
        prepare_indexed_night_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(1, sizeof(ReportIndexedNight), false));
        if (!prepare_indexed_night_) {
            log_report_alloc_failed("prepare_indexed_night",
                                    sizeof(ReportIndexedNight));
            return ResultPrepareOutcome::Retry;
        }
    }
    size_t current_therapy_index = therapy_index;
    memset(prepare_indexed_night_, 0, sizeof(*prepare_indexed_night_));
    if (!indexed_night_by_start(night_start_ms,
                                *prepare_indexed_night_,
                                &current_therapy_index)) {
        clear_result_prepare();
        fail_result_prepare("night_not_found");
        return ResultPrepareOutcome::Failed;
    }
    const ReportIndexedNight &indexed_night = *prepare_indexed_night_;
    therapy_index = current_therapy_index;
    const ReportSummaryRecord &night = indexed_night.summary;
    if (indexed_night.edf_catalog_pending) {
        result_status_.state = ReportResultState::Preparing;
        result_status_.therapy_index = therapy_index;
        result_status_.night_start_ms = night.start_ms;
        result_status_.night_end_ms = night.end_ms;
        result_status_.duration_min = night.duration_min;
        result_status_.error = "edf_catalog_refreshing";
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result prepare deferred for EDF catalog "
                  "night=%llu index=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index));
        return ResultPrepareOutcome::Deferred;
    }

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        result_status_.error = "summary_lock_busy";
        return ResultPrepareOutcome::Retry;
    }
    format_night_etag_unlocked(night,
                               indexed_night.source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();
    if (!current_etag[0]) {
        result_status_.error = "empty_etag";
        return ResultPrepareOutcome::Retry;
    }

    if (publish_existing_result_if_current(therapy_index,
                                           indexed_night,
                                           current_etag,
                                           refresh_cache)) {
        return ResultPrepareOutcome::Prepared;
    }

    begin_result_prepare_for_night(therapy_index, indexed_night, current_etag);
    result_skip_plot_cache_ = refresh_cache;

    // Use the session data span, not night.end_ms (a 24h day bucket far past the
    // therapy data) coverage is only written/checked over the session span,
    // so the result chunk range and its coverage check must match.
    ReportSessionRange night_range;
    if (!result_data_span(night_range.start_ms, night_range.end_ms)) {
        result_status_.state = ReportResultState::Incomplete;
        result_status_.error = "no_sessions";
        return publish_result_to_slot() ? ResultPrepareOutcome::Prepared
                                        : ResultPrepareOutcome::Retry;
    }

    bool deferred = false;
    if (!resolve_and_materialize_result_for_night(indexed_night,
                                                  night_range.start_ms,
                                                  night_range.end_ms,
                                                  &deferred)) {
        release_result_edf_sessions();
        return result_status_.state == ReportResultState::Error
                   ? ResultPrepareOutcome::Failed
                   : ResultPrepareOutcome::Retry;
    }
    if (deferred) return ResultPrepareOutcome::Deferred;
    const bool uses_edf = result_uses_edf_provider();
    if (!uses_edf) {
        release_result_edf_sessions();
    }

    deferred = false;
    if (!refresh_result_cache_if_needed(indexed_night,
                                        therapy_index,
                                        refresh_cache,
                                        deferred)) {
        release_result_edf_sessions();
        return result_status_.state == ReportResultState::Error
                   ? ResultPrepareOutcome::Failed
                   : ResultPrepareOutcome::Retry;
    }
    if (deferred) {
        release_result_edf_sessions();
        return ResultPrepareOutcome::Deferred;
    }

    const bool ok = finalize_result_prepare(therapy_index);
    if (!plot_build_active_) {
        release_result_edf_sessions();
    }
    if (ok) return ResultPrepareOutcome::Prepared;
    return result_status_.state == ReportResultState::Error
               ? ResultPrepareOutcome::Failed
               : ResultPrepareOutcome::Retry;
}

}  // namespace aircannect
