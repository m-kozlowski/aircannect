#include "report_manager.h"
#include "report_plot_prebuild_service.h"

#include "background_worker.h"
#include "debug_log.h"
#include "edf_report_catalog.h"
#include "edf_report_catalog_job.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_result_cache_files.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

using BuildQueueResult = ReportBuildRuntime::BuildQueueResult;

bool indexed_night_by_newest_cursor(const ReportIndexedNight *nights,
                                    size_t count,
                                    size_t cursor,
                                    ReportIndexedNight &out,
                                    size_t &therapy_index) {
    if (!nights) return false;

    size_t seen = 0;
    for (size_t i = count; i > 0; --i) {
        const ReportIndexedNight &night = nights[i - 1];
        if (!night.summary.valid ||
            night.summary.start_ms == 0 ||
            night.summary.duration_min == 0) {
            continue;
        }

        if (seen == cursor) {
            out = night;
            therapy_index = seen;
            return true;
        }

        seen++;
    }

    return false;
}

class ScopedIndexedNightList {
public:
    ScopedIndexedNightList(const char *context, size_t capacity)
        : context_(context),
          capacity_(capacity),
          nights_(static_cast<ReportIndexedNight *>(Memory::alloc_large(
              capacity * sizeof(ReportIndexedNight),
              false))) {
        if (!nights_) {
            log_report_alloc_failed(context_,
                                    capacity * sizeof(ReportIndexedNight));
        }
    }

    ~ScopedIndexedNightList() {
        Memory::free(nights_);
    }

    ScopedIndexedNightList(const ScopedIndexedNightList &) = delete;
    ScopedIndexedNightList &operator=(const ScopedIndexedNightList &) = delete;

    explicit operator bool() const { return nights_ != nullptr; }
    ReportIndexedNight *data() { return nights_; }
    const ReportIndexedNight *data() const { return nights_; }
    size_t capacity() const { return capacity_; }

private:
    const char *context_ = nullptr;
    size_t capacity_ = 0;
    ReportIndexedNight *nights_ = nullptr;
};

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

    if (!build_.has_capacity()) {
        return ReportPlotPrebuildResult::Waiting;
    }

    ScopedIndexedNightList snapshot("report_night_index_prebuild",
                                    AC_REPORT_SUMMARY_RECORD_MAX);
    if (!snapshot) return ReportPlotPrebuildResult::Unavailable;

    size_t snapshot_count = 0;
    if (!night_index_.build(snapshot.data(),
                            snapshot.capacity(),
                            snapshot_count)) {
        return ReportPlotPrebuildResult::Waiting;
    }

    constexpr size_t SCAN_STEPS_PER_CALL = 4;
    for (size_t step = 0; step < SCAN_STEPS_PER_CALL; ++step) {
        ReportIndexedNight night;
        size_t therapy_index = 0;
        const bool found =
            indexed_night_by_newest_cursor(snapshot.data(),
                                           snapshot_count,
                                           build_.prebuild_cursor(),
                                           night,
                                           therapy_index);
        if (!found) {
            build_.mark_prebuild_drained(
                now_ms,
                AC_REPORT_PLOT_PREBUILD_RESCAN_MS);
            return ReportPlotPrebuildResult::Drained;
        }

        build_.advance_prebuild_cursor();
        if (night.edf_catalog_pending) return ReportPlotPrebuildResult::Waiting;

        char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
        night_index_.format_result_etag(night, etag, sizeof(etag));

        if (result_plot_cache_exists_for_etag(night.summary.start_ms, etag)) {
            continue;
        }

        const BuildQueueResult queued =
            build_.enqueue(night.summary.start_ms,
                           therapy_index,
                           false,
                           true);
        if (queued == BuildQueueResult::Queued) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Idle plot prebuild queued night=%llu index=%lu\n",
                      static_cast<unsigned long long>(
                          night.summary.start_ms),
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
