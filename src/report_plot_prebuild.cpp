#include "report_manager.h"
#include "report_plot_prebuild_service.h"

#include "background_worker.h"
#include "debug_log.h"
#include "edf_report_catalog.h"
#include "edf_report_catalog_job.h"
#include "report_index_scratch.h"
#include "report_manager_limits.h"
#include "report_result_cache_files.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

using BuildQueueResult = ReportBuildRuntime::BuildQueueResult;

}  // namespace

ReportPlotPrebuildService::ReportPlotPrebuildService(
    ReportSummaryService &summary,
    ReportCacheFetchService &cache_fetch,
    ReportResultBuildService &result_build,
    ReportRangePlotBuilder &range_plot,
    ReportResultCacheRuntime &result_cache,
    ReportNightIndexService &night_index,
    ReportBuildRuntime &build)
    : summary_(summary),
      cache_fetch_(cache_fetch),
      result_build_(result_build),
      range_plot_(range_plot),
      result_cache_(result_cache),
      night_index_(night_index),
      build_(build) {}

bool ReportPlotPrebuildService::gate_open(const char **reason) const {
    BackgroundWorker *worker = background_worker();
    if (!worker) {
        if (reason) *reason = "no_worker";
        return false;
    }

    return worker->idle_gate_open(reason);
}

ReportPlotPrebuildResult ReportPlotPrebuildService::request() {
    const char *gate_reason = "idle";
    if (!gate_open(&gate_reason)) {
        return ReportPlotPrebuildResult::Waiting;
    }

    if (summary_.active() || cache_fetch_.active() ||
        result_build_.plot_builder().active() ||
        range_plot_.active() || result_cache_.writer_active()) {
        return ReportPlotPrebuildResult::Waiting;
    }

    {
        Storage::Guard g;
        if (!Storage::mounted()) return ReportPlotPrebuildResult::Unavailable;
    }

    ReportNightIndexCacheKey cache_key;
    if (!night_index_.cache_key(cache_key)) {
        return ReportPlotPrebuildResult::Waiting;
    }

    if (cache_key.catalog_present &&
        cache_key.catalog_state !=
            static_cast<uint8_t>(EdfReportCatalogState::Ready) &&
        cache_key.catalog_state !=
            static_cast<uint8_t>(EdfReportCatalogState::Error)) {
        return ReportPlotPrebuildResult::Waiting;
    }

    const uint32_t now_ms = millis();
    if (!build_.prebuild_key_matches(cache_key)) {
        build_.reset_prebuild_for_key(cache_key);
    } else if (build_.prebuild_rescan_delay_active(now_ms)) {
        return ReportPlotPrebuildResult::Drained;
    }

    ReportNightIndexSnapshotRef snapshot;
    const ReportNightIndexSnapshotResult snapshot_result =
        night_index_.snapshot(snapshot);
    if (snapshot_result == ReportNightIndexSnapshotResult::Busy) {
        return ReportPlotPrebuildResult::Waiting;
    }
    if (snapshot_result != ReportNightIndexSnapshotResult::Ready ||
        !snapshot) {
        return ReportPlotPrebuildResult::Unavailable;
    }

    ScopedIndexedNight night("report_night_index_prebuild");
    if (!night) return ReportPlotPrebuildResult::Unavailable;

    constexpr size_t SCAN_STEPS_PER_CALL = 4;
    for (size_t step = 0; step < SCAN_STEPS_PER_CALL; ++step) {
        const size_t therapy_index = build_.prebuild_cursor();
        if (!snapshot->by_therapy_index(therapy_index, night.get())) {
            build_.mark_prebuild_drained(
                now_ms,
                AC_REPORT_PLOT_PREBUILD_RESCAN_MS);
            return ReportPlotPrebuildResult::Drained;
        }

        build_.advance_prebuild_cursor();
        if (night->edf_catalog_pending) {
            return ReportPlotPrebuildResult::Waiting;
        }

        char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
        night_index_.format_result_etag(night.get(), etag, sizeof(etag));

        bool pair_exists = result_cache_pair_exists_for_etag(
            night->summary.start_ms,
            etag);
        if (pair_exists && therapy_index < AC_REPORT_RESULT_SLOT_MAX) {
            const ReportCacheLoadRequest load = result_cache_.request_load(
                night->summary.start_ms,
                etag);

            if (load == ReportCacheLoadRequest::Queued ||
                load == ReportCacheLoadRequest::Pending) {
                return ReportPlotPrebuildResult::Waiting;
            }

            if (load == ReportCacheLoadRequest::Full ||
                load == ReportCacheLoadRequest::Failed ||
                load == ReportCacheLoadRequest::Unavailable) {
                build_.rewind_prebuild_cursor();
                return ReportPlotPrebuildResult::Waiting;
            }

            if (load == ReportCacheLoadRequest::Missing) {
                pair_exists = false;
            }
        }

        if (pair_exists) {
            continue;
        }

        if (!build_.has_capacity()) {
            build_.rewind_prebuild_cursor();
            return ReportPlotPrebuildResult::Waiting;
        }

        const BuildQueueResult queued =
            build_.enqueue(night->summary.start_ms,
                           therapy_index,
                           false,
                           true);
        if (queued == BuildQueueResult::Queued) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Idle plot prebuild queued night=%llu index=%lu\n",
                      static_cast<unsigned long long>(
                          night->summary.start_ms),
                      static_cast<unsigned long>(therapy_index));
            return ReportPlotPrebuildResult::Queued;
        }

        if (queued == BuildQueueResult::AlreadyQueued) {
            return ReportPlotPrebuildResult::AlreadyQueued;
        }

        if (queued == BuildQueueResult::Full) {
            build_.rewind_prebuild_cursor();
            return ReportPlotPrebuildResult::Waiting;
        }

        return ReportPlotPrebuildResult::Unavailable;
    }

    return ReportPlotPrebuildResult::Scanned;
}

void ReportPlotPrebuildService::preempt() {
    result_build_.plot_builder().preempt_idle_prebuild();
}

ReportManager::PlotPrebuildResult ReportManager::request_idle_plot_prebuild() {
    return plot_prebuild_.request();
}

void ReportManager::preempt_idle_plot_prebuild() {
    plot_prebuild_.preempt();
}

}  // namespace aircannect
