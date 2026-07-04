#include "report_manager.h"

#include <stdio.h>

#include "background_worker.h"
#include "debug_log.h"
#include "edf_report_catalog.h"
#include "edf_report_catalog_job.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

const char *result_prepare_outcome_name(uint8_t outcome) {
    switch (outcome) {
        case 0:
            return "prepared";
        case 1:
            return "deferred";
        case 2:
            return "retry";
        case 3:
            return "failed";
    }
    return "unknown";
}

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

ReportManager::BuildQueueSnapshot ReportManager::build_queue_snapshot() const {
    BuildQueueSnapshot snap;
    snap.available = build_queue_lock_ != nullptr;
    if (!build_queue_lock_) return snap;
    if (xSemaphoreTake(build_queue_lock_, 0) != pdTRUE) return snap;

    const uint32_t now_ms = millis();

    snap.lock_ok = true;
    snap.count = build_queue_count_;
    if (build_queue_count_ > 0) {
        const ResultBuildJob &job = build_queue_[build_queue_head_];
        snap.head_night_ms = job.night_start_ms;
        snap.head_therapy_index = job.therapy_index;
        snap.head_refresh = job.refresh;
        snap.head_idle_prebuild = job.idle_prebuild;
        snap.head_age_ms = job.queued_ms ? now_ms - job.queued_ms : 0;
    }

    snap.last_night_ms = build_queue_last_night_ms_;
    snap.last_therapy_index = build_queue_last_therapy_index_;
    snap.enqueue_total = build_queue_enqueue_total_;
    snap.queued_total = build_queue_queued_total_;
    snap.already_total = build_queue_already_total_;
    snap.service_total = build_queue_service_total_;
    snap.last_enqueue_night_ms = build_queue_last_enqueue_night_ms_;
    snap.last_enqueue_therapy_index = build_queue_last_enqueue_therapy_index_;
    copy_cstr(snap.last_read, sizeof(snap.last_read), build_queue_last_read_);
    copy_cstr(snap.last_enqueue_result,
              sizeof(snap.last_enqueue_result),
              build_queue_last_enqueue_result_);
    copy_cstr(snap.last_service_block,
              sizeof(snap.last_service_block),
              build_queue_last_service_block_);
    copy_cstr(snap.last_outcome,
              sizeof(snap.last_outcome),
              build_queue_last_outcome_);
    copy_cstr(snap.last_state, sizeof(snap.last_state), build_queue_last_state_);
    copy_cstr(snap.last_error, sizeof(snap.last_error), build_queue_last_error_);

    xSemaphoreGive(build_queue_lock_);
    return snap;
}

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

ReportManager::BuildQueueResult ReportManager::enqueue_build(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh,
    bool idle_prebuild) {
    if (!build_queue_lock_ || !night_start_ms) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result build enqueue rejected night=%llu index=%lu "
                  "refresh=%u idle_prebuild=%u reason=unavailable\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index),
                  refresh ? 1u : 0u,
                  idle_prebuild ? 1u : 0u);
        return BuildQueueResult::Unavailable;
    }

    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

    build_queue_enqueue_total_++;
    build_queue_last_enqueue_night_ms_ = night_start_ms;
    build_queue_last_enqueue_therapy_index_ = therapy_index;
    copy_cstr(build_queue_last_service_block_,
              sizeof(build_queue_last_service_block_),
              "");

    for (size_t k = 0; k < build_queue_count_; ++k) {
        size_t idx = (build_queue_head_ + k) % AC_REPORT_BUILD_QUEUE_MAX;
        if (build_queue_[idx].night_start_ms != night_start_ms) continue;

        build_queue_[idx].therapy_index = therapy_index;
        if (refresh) {
            build_queue_[idx].refresh = true;
            build_queue_[idx].next_attempt_ms = 0;
        }
        if (!idle_prebuild) {
            if (build_queue_[idx].idle_prebuild) {
                build_queue_[idx].next_attempt_ms = 0;
            }
            build_queue_[idx].idle_prebuild = false;
        }

        build_queue_already_total_++;
        copy_cstr(build_queue_last_enqueue_result_,
                  sizeof(build_queue_last_enqueue_result_),
                  "already");
        const size_t count = build_queue_count_;

        xSemaphoreGive(build_queue_lock_);

        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result build already queued night=%llu index=%lu "
                  "refresh=%u idle_prebuild=%u count=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index),
                  refresh ? 1u : 0u,
                  idle_prebuild ? 1u : 0u,
                  static_cast<unsigned long>(count));
        return BuildQueueResult::AlreadyQueued;
    }

    if (build_queue_count_ < AC_REPORT_BUILD_QUEUE_MAX) {
        size_t tail =
            (build_queue_head_ + build_queue_count_) % AC_REPORT_BUILD_QUEUE_MAX;
        build_queue_[tail].night_start_ms = night_start_ms;
        build_queue_[tail].therapy_index = therapy_index;
        build_queue_[tail].refresh = refresh;
        build_queue_[tail].idle_prebuild = idle_prebuild;
        build_queue_[tail].queued_ms = millis();
        build_queue_[tail].next_attempt_ms = 0;
        build_queue_count_++;
        build_queue_queued_total_++;
        copy_cstr(build_queue_last_enqueue_result_,
                  sizeof(build_queue_last_enqueue_result_),
                  "queued");
        const size_t count = build_queue_count_;

        xSemaphoreGive(build_queue_lock_);

        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result build queued night=%llu index=%lu refresh=%u "
                  "idle_prebuild=%u count=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index),
                  refresh ? 1u : 0u,
                  idle_prebuild ? 1u : 0u,
                  static_cast<unsigned long>(count));
        return BuildQueueResult::Queued;
    }

    copy_cstr(build_queue_last_enqueue_result_,
              sizeof(build_queue_last_enqueue_result_),
              "full");
    xSemaphoreGive(build_queue_lock_);

    Log::logf(CAT_REPORT,
              idle_prebuild ? LOG_DEBUG : LOG_WARN,
              "Result build enqueue rejected night=%llu index=%lu "
              "refresh=%u idle_prebuild=%u reason=full count=%lu\n",
              static_cast<unsigned long long>(night_start_ms),
              static_cast<unsigned long>(therapy_index),
              refresh ? 1u : 0u,
              idle_prebuild ? 1u : 0u,
              static_cast<unsigned long>(AC_REPORT_BUILD_QUEUE_MAX));
    return BuildQueueResult::Full;
}

bool ReportManager::build_queue_has_capacity() const {
    if (!build_queue_lock_) return false;
    if (xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    const bool available = build_queue_count_ < AC_REPORT_BUILD_QUEUE_MAX;

    xSemaphoreGive(build_queue_lock_);
    return available;
}

void ReportManager::clear_build_queue(uint64_t night_start_ms, bool all) {
    if (!build_queue_lock_) return;

    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

    ResultBuildJob kept[AC_REPORT_BUILD_QUEUE_MAX];
    size_t kept_count = 0;
    for (size_t k = 0; k < build_queue_count_; ++k) {
        const size_t idx = (build_queue_head_ + k) % AC_REPORT_BUILD_QUEUE_MAX;
        const ResultBuildJob &job = build_queue_[idx];
        if (!all && job.night_start_ms != night_start_ms) {
            kept[kept_count++] = job;
        }
    }

    for (size_t i = 0; i < AC_REPORT_BUILD_QUEUE_MAX; ++i) {
        build_queue_[i] = i < kept_count ? kept[i] : ResultBuildJob{};
    }
    build_queue_head_ = 0;
    build_queue_count_ = kept_count;

    xSemaphoreGive(build_queue_lock_);
}

void ReportManager::service_build_queue(bool realtime_active) {
    if (!build_queue_lock_) return;

    // Existing report work owns the materialization pipeline.
    if (summary_fetch_active_ || plot_build_active_ || range_build_active_) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0) {
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      summary_fetch_active_
                          ? "summary"
                          : (plot_build_active_ ? "plot" : "range"));
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    // Realtime stream/therapy work preempts report materialization.
    if (realtime_active) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0) {
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      "realtime");
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

    const bool have = build_queue_count_ > 0;
    ResultBuildJob job =
        have ? build_queue_[build_queue_head_] : ResultBuildJob{};

    xSemaphoreGive(build_queue_lock_);

    if (!have) return;

    const uint32_t now_ms = millis();
    if (job.next_attempt_ms != 0 &&
        static_cast<int32_t>(now_ms - job.next_attempt_ms) < 0) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0) {
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      "retry_wait");
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    if (job.idle_prebuild) {
        const char *reason = "idle";
        if (!idle_prebuild_gate_open(&reason)) {
            xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      reason ? reason : "gate");
            xSemaphoreGive(build_queue_lock_);
            return;
        }
    }

    if (cache_fetch_active_) {
        if (!job.idle_prebuild) prefetch_yield_to_foreground();

        if (cache_fetch_active_) {
            xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      "cache_fetch");
            xSemaphoreGive(build_queue_lock_);
            return;
        }
    }

    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    build_queue_service_total_++;
    copy_cstr(build_queue_last_service_block_,
              sizeof(build_queue_last_service_block_),
              "");
    xSemaphoreGive(build_queue_lock_);

    active_build_idle_prebuild_ = job.idle_prebuild;

    const ResultPrepareOutcome outcome =
        prepare_result_by_night_start_internal(job.night_start_ms,
                                               job.therapy_index,
                                               job.refresh);

    active_build_idle_prebuild_ = false;

    const char *outcome_name =
        result_prepare_outcome_name(static_cast<uint8_t>(outcome));

    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    build_queue_last_night_ms_ = job.night_start_ms;
    build_queue_last_therapy_index_ = job.therapy_index;
    copy_cstr(build_queue_last_outcome_,
              sizeof(build_queue_last_outcome_),
              outcome_name);
    copy_cstr(build_queue_last_state_,
              sizeof(build_queue_last_state_),
              result_state_name());
    copy_cstr(build_queue_last_error_,
              sizeof(build_queue_last_error_),
              result_status_.error.c_str());
    xSemaphoreGive(build_queue_lock_);

    Log::logf(CAT_REPORT,
              outcome == ResultPrepareOutcome::Failed ? LOG_WARN : LOG_DEBUG,
              "Result build step night=%llu index=%lu refresh=%u "
              "idle_prebuild=%u outcome=%s state=%s error=%s chunks=%lu "
              "records=%lu bytes=%lu\n",
              static_cast<unsigned long long>(job.night_start_ms),
              static_cast<unsigned long>(job.therapy_index),
              job.refresh ? 1u : 0u,
              job.idle_prebuild ? 1u : 0u,
              outcome_name,
              result_state_name(),
              result_status_.error.length() ? result_status_.error.c_str()
                                            : "--",
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(result_status_.record_count),
              static_cast<unsigned long>(result_status_.payload_bytes));

    if (outcome == ResultPrepareOutcome::Deferred ||
        outcome == ResultPrepareOutcome::Retry) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0 &&
            build_queue_[build_queue_head_].night_start_ms ==
                job.night_start_ms) {
            build_queue_[build_queue_head_].next_attempt_ms =
                millis() + AC_BG_WORKER_BUSY_RECHECK_MS;
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

    if (build_queue_count_ > 0 &&
        build_queue_[build_queue_head_].night_start_ms == job.night_start_ms) {
        build_queue_head_ = (build_queue_head_ + 1) % AC_REPORT_BUILD_QUEUE_MAX;
        build_queue_count_--;
    }

    xSemaphoreGive(build_queue_lock_);
}

}  // namespace aircannect
