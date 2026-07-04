#include "report_manager.h"

#include "debug_log.h"
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
