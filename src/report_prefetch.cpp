#include "report_manager.h"

#include "debug_log.h"
#include "report_cache_fetch_service.h"
#include "report_edf_catalog_context.h"
#include "report_night_cache_service.h"
#include "report_night_coverage.h"
#include "report_prefetch_runtime.h"
#include "report_prefetch_service.h"
#include "report_range_plot_builder.h"
#include "report_result_build_service.h"
#include "report_sources.h"
#include "report_summary_service.h"

namespace aircannect {
namespace {

bool night_in_prefetch_cooldown(uint64_t night_start_ms,
                                uint32_t now_ms,
                                const void *context) {
    const ReportPrefetchRuntime *prefetch =
        static_cast<const ReportPrefetchRuntime *>(context);
    return prefetch && prefetch->in_cooldown(night_start_ms, now_ms);
}

}  // namespace

ReportPrefetchService::ReportPrefetchService(
    ReportPrefetchRuntime &prefetch,
    ReportCacheFetchService &cache_fetch,
    ReportNightCacheService &night_cache,
    ReportSummaryService &summary,
    ReportResultBuildService &result_build,
    ReportRangePlotBuilder &range_plot,
    ReportEdfCatalogContext &edf_catalog)
    : prefetch_(prefetch),
      cache_fetch_(cache_fetch),
      night_cache_(night_cache),
      summary_(summary),
      result_build_(result_build),
      range_plot_(range_plot),
      edf_catalog_(edf_catalog) {}

void ReportPrefetchService::set_phase(ReportPrefetchPhase phase,
                                      uint64_t night_ms,
                                      bool inc_completed,
                                      bool inc_failed) {
    const char *source = "";
    const char *error = "";
    if (inc_failed) {
        const ReportCacheFetchStatus &status = cache_fetch_.status();
        source = status.source_count
                     ? report_source_spool_type(status.active_source)
                     : "";
        error = status.error.length() ? status.error.c_str() : "";
    }

    prefetch_.set_phase(phase,
                        night_ms,
                        inc_completed,
                        inc_failed,
                        source,
                        error);
}

bool ReportPrefetchService::busy() const {
    return summary_.active() || cache_fetch_.active() ||
           result_build_.plot_active() || range_plot_.active();
}

bool ReportPrefetchService::foreground_busy() const {
    if (!cache_fetch_.active()) return false;

    // A cache fetch is in flight: it's foreground unless it's the prefetch's own.
    return !prefetch_.is_fetching();
}

bool ReportPrefetchService::work_active() const {
    const ReportPrefetchSnapshot snap = prefetch_.snapshot();
    const ReportPrefetchPhase phase = snap.phase;

    return phase == ReportPrefetchPhase::Selecting ||
           phase == ReportPrefetchPhase::Pending ||
           phase == ReportPrefetchPhase::Fetching ||
           phase == ReportPrefetchPhase::Done;
}

bool ReportPrefetchService::request_candidate() {
    return prefetch_.request_candidate();
}

void ReportPrefetchService::preempt() {
    prefetch_.preempt();
}

ReportPrefetchSnapshot ReportPrefetchService::snapshot() const {
    return prefetch_.snapshot();
}

void ReportPrefetchService::yield_to_foreground() {
    if (!prefetch_.is_fetching()) return;

    if (cache_fetch_.active()) {
        (void)cache_fetch_.cancel("preempted_by_user");
    }

    prefetch_.set_phase(ReportPrefetchPhase::Idle,
                        0,
                        false,
                        false,
                        "",
                        "");
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "prefetch yielded to foreground prepare\n");
}

void ReportPrefetchService::service(bool realtime_active) {
    const ReportPrefetchServiceState state =
        prefetch_.take_service_state();

    const ReportPrefetchPhase phase = state.phase;
    const bool preempt = state.preempt;
    const uint64_t active = state.active_night_ms;

    if (preempt && (phase == ReportPrefetchPhase::Selecting ||
                    phase == ReportPrefetchPhase::Fetching ||
                    phase == ReportPrefetchPhase::Pending)) {
        if (cache_fetch_.active()) {
            (void)cache_fetch_.cancel("prefetch_preempted");
        }
        set_phase(ReportPrefetchPhase::Idle, 0, false, false);
        return;
    }

    if (phase == ReportPrefetchPhase::Selecting) {
        if (realtime_active || busy()) return;
        if (edf_catalog_.pending_or_request_refresh()) return;

        uint64_t night = 0;
        if (night_cache_.next_needing_cache(night,
                                            night_in_prefetch_cooldown,
                                            &prefetch_) &&
            night != 0) {
            set_phase(ReportPrefetchPhase::Pending, night, false, false);
        } else {
            set_phase(ReportPrefetchPhase::Drained, 0, false, false);
        }
        return;
    }

    if (phase == ReportPrefetchPhase::Fetching && !cache_fetch_.active()) {
        ReportNightCoverageStatus coverage;
        const bool covered =
            night_cache_.coverage(active, coverage) &&
            coverage.missing_required == 0;

        if (!covered) prefetch_.note_failure(active);
        set_phase(covered ? ReportPrefetchPhase::Done
                          : ReportPrefetchPhase::Failed,
                  active,
                  covered,
                  !covered);
        return;
    }

    if (realtime_active) {
        if (phase == ReportPrefetchPhase::Fetching && cache_fetch_.active()) {
            (void)cache_fetch_.cancel("preempted_by_stream");
            set_phase(ReportPrefetchPhase::Idle, 0, false, false);
            Log::logf(CAT_REPORT, LOG_DEBUG,
                      "prefetch yielded to stream activity\n");
        }
        return;
    }

    if (phase == ReportPrefetchPhase::Pending && !busy()) {
        if (active != 0 && night_cache_.request_cache(active, false)) {
            set_phase(ReportPrefetchPhase::Fetching,
                      active,
                      false,
                      false);
        } else if (active != 0) {
            set_phase(ReportPrefetchPhase::Failed, active, false, true);
        } else {
            set_phase(ReportPrefetchPhase::Drained, 0, false, false);
        }
    }
}

bool ReportManager::prefetch_request_candidate() {
    return prefetch_service_.request_candidate();
}

void ReportManager::prefetch_preempt() {
    prefetch_service_.preempt();
}

ReportManager::PrefetchSnapshot ReportManager::prefetch_snapshot() const {
    return prefetch_service_.snapshot();
}

}  // namespace aircannect
