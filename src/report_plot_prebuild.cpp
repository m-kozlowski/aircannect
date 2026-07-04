#include "report_manager.h"

#include "background_worker.h"
#include "debug_log.h"
#include "edf_report_catalog.h"
#include "edf_report_catalog_job.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

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

bool ReportManager::idle_prebuild_gate_open(const char **reason) const {
    BackgroundWorker *worker = background_worker();
    if (!worker) {
        if (reason) *reason = "no_worker";
        return false;
    }

    return worker->idle_gate_open(reason);
}

bool ReportManager::plot_prebuild_key_matches(
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    return plot_prebuild_key_valid_ &&
           plot_prebuild_summary_revision_ == summary_revision &&
           plot_prebuild_catalog_present_ == catalog_present &&
           plot_prebuild_catalog_state_ == catalog_state &&
           plot_prebuild_catalog_refresh_id_ == catalog_refresh_id;
}

void ReportManager::set_plot_prebuild_key(uint32_t summary_revision,
                                          bool catalog_present,
                                          uint8_t catalog_state,
                                          uint32_t catalog_refresh_id) {
    plot_prebuild_key_valid_ = true;
    plot_prebuild_summary_revision_ = summary_revision;
    plot_prebuild_catalog_present_ = catalog_present;
    plot_prebuild_catalog_state_ = catalog_state;
    plot_prebuild_catalog_refresh_id_ = catalog_refresh_id;
    plot_prebuild_cursor_ = 0;
    plot_prebuild_next_scan_ms_ = 0;
}

ReportManager::PlotPrebuildResult ReportManager::request_idle_plot_prebuild() {
    const char *gate_reason = "idle";
    if (!idle_prebuild_gate_open(&gate_reason)) {
        return PlotPrebuildResult::Waiting;
    }

    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_ || plot_cache_writer_active()) {
        return PlotPrebuildResult::Waiting;
    }

    {
        Storage::Guard g;
        if (!Storage::mounted()) return PlotPrebuildResult::Unavailable;
    }

    uint32_t summary_revision = 0;
    bool catalog_present = false;
    uint8_t catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Idle);
    uint32_t catalog_refresh_id = 0;
    if (!index_cache_key(summary_revision,
                         catalog_present,
                         catalog_state,
                         catalog_refresh_id)) {
        return PlotPrebuildResult::Waiting;
    }

    if (catalog_present &&
        catalog_state != static_cast<uint8_t>(EdfReportCatalogState::Ready) &&
        catalog_state != static_cast<uint8_t>(EdfReportCatalogState::Error)) {
        return PlotPrebuildResult::Waiting;
    }

    if (!plot_prebuild_key_matches(summary_revision,
                                   catalog_present,
                                   catalog_state,
                                   catalog_refresh_id)) {
        set_plot_prebuild_key(summary_revision,
                              catalog_present,
                              catalog_state,
                              catalog_refresh_id);
    } else if (plot_prebuild_next_scan_ms_ != 0 &&
               static_cast<int32_t>(millis() -
                                    plot_prebuild_next_scan_ms_) < 0) {
        return PlotPrebuildResult::Drained;
    }

    if (!build_queue_has_capacity()) {
        return PlotPrebuildResult::Waiting;
    }

    ScopedIndexedNightList snapshot("report_night_index_prebuild",
                                    AC_REPORT_SUMMARY_RECORD_MAX);
    if (!snapshot) return PlotPrebuildResult::Unavailable;

    size_t snapshot_count = 0;
    if (!build_indexed_nights(snapshot.data(),
                              snapshot.capacity(),
                              snapshot_count)) {
        return PlotPrebuildResult::Waiting;
    }

    constexpr size_t SCAN_STEPS_PER_CALL = 4;
    for (size_t step = 0; step < SCAN_STEPS_PER_CALL; ++step) {
        ReportIndexedNight night;
        size_t therapy_index = 0;
        const bool found =
            indexed_night_by_newest_cursor(snapshot.data(),
                                           snapshot_count,
                                           plot_prebuild_cursor_,
                                           night,
                                           therapy_index);
        if (!found) {
            plot_prebuild_next_scan_ms_ =
                millis() + AC_REPORT_PLOT_PREBUILD_RESCAN_MS;
            if (plot_prebuild_next_scan_ms_ == 0) {
                plot_prebuild_next_scan_ms_ = 1;
            }
            return PlotPrebuildResult::Drained;
        }

        plot_prebuild_cursor_++;
        if (night.edf_catalog_pending) return PlotPrebuildResult::Waiting;

        char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
        if (!take_summary_lock(pdMS_TO_TICKS(20))) {
            return PlotPrebuildResult::Waiting;
        }
        format_night_etag_unlocked(night.summary,
                                   night.source_signature,
                                   etag,
                                   sizeof(etag));
        give_summary_lock();

        if (result_plot_cache_exists_for_night(night, etag)) {
            continue;
        }

        const BuildQueueResult queued =
            enqueue_build(night.summary.start_ms,
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
            return PlotPrebuildResult::Queued;
        }

        if (queued == BuildQueueResult::AlreadyQueued) {
            return PlotPrebuildResult::AlreadyQueued;
        }

        if (queued == BuildQueueResult::Full) {
            if (plot_prebuild_cursor_ > 0) plot_prebuild_cursor_--;
            return PlotPrebuildResult::Waiting;
        }

        return PlotPrebuildResult::Unavailable;
    }

    return PlotPrebuildResult::Scanned;
}

void ReportManager::preempt_idle_plot_prebuild() {
    if (!plot_build_active_ || !plot_build_idle_prebuild_) return;

    const uint32_t elapsed_ms =
        plot_build_started_ms_
            ? static_cast<uint32_t>(millis() - plot_build_started_ms_)
            : 0;

    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Idle plot prebuild preempted night=%llu elapsed_ms=%lu\n",
              static_cast<unsigned long long>(
                  plot_build_night_start_ms_.load()),
              static_cast<unsigned long>(elapsed_ms));

    reset_plot_build();
    release_result_edf_sessions();
}

}  // namespace aircannect
