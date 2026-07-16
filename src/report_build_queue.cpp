#include "report_manager.h"

#include <Arduino.h>

#include "board_report.h"
#include "debug_log.h"
#include "report_build_queue_service.h"
#include "report_index_scratch.h"
#include "string_util.h"

namespace aircannect {
namespace {

using ResultBuildJob = ReportBuildRuntime::ResultBuildJob;

}  // namespace

ReportBuildQueue::~ReportBuildQueue() {
    if (!lock_) return;

    vSemaphoreDelete(lock_);
    lock_ = nullptr;
}

bool ReportBuildQueue::begin() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

ReportBuildQueueSnapshot ReportBuildQueue::snapshot() const {
    ReportBuildQueueSnapshot snap;
    snap.available = lock_ != nullptr;
    if (!lock_) return snap;
    if (xSemaphoreTake(lock_, 0) != pdTRUE) return snap;

    const uint32_t now_ms = millis();

    snap.lock_ok = true;
    snap.count = count_;
    if (count_ > 0) {
        const ResultBuildJob &job = queue_[0];
        snap.head_night_ms = job.night_start_ms;
        snap.head_therapy_index = job.therapy_index;
        snap.head_refresh = job.refresh;
        snap.head_idle_prebuild = job.idle_prebuild;
        snap.head_age_ms = job.queued_ms ? now_ms - job.queued_ms : 0;

        const bool waiting = job.next_attempt_ms != 0 &&
            static_cast<int32_t>(now_ms - job.next_attempt_ms) < 0;
        snap.head_wait_ms = waiting ? job.next_attempt_ms - now_ms : 0;
        snap.head_defer_count = job.defer_count;
        snap.head_retry_attempts = job.retry_attempts;
    }

    snap.last_night_ms = last_night_ms_;
    snap.last_therapy_index = last_therapy_index_;
    snap.enqueue_total = enqueue_total_;
    snap.queued_total = queued_total_;
    snap.already_total = already_total_;
    snap.service_total = service_total_;
    snap.last_enqueue_night_ms = last_enqueue_night_ms_;
    snap.last_enqueue_therapy_index = last_enqueue_therapy_index_;
    copy_cstr(snap.last_read, sizeof(snap.last_read), last_read_);
    copy_cstr(snap.last_enqueue_result,
              sizeof(snap.last_enqueue_result),
              last_enqueue_result_);
    copy_cstr(snap.last_service_block,
              sizeof(snap.last_service_block),
              last_service_block_);
    copy_cstr(snap.last_outcome, sizeof(snap.last_outcome), last_outcome_);
    copy_cstr(snap.last_state, sizeof(snap.last_state), last_state_);
    copy_cstr(snap.last_error, sizeof(snap.last_error), last_error_);

    xSemaphoreGive(lock_);
    return snap;
}

ReportBuildQueue::BuildQueueResult ReportBuildQueue::enqueue(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh,
    bool idle_prebuild) {
    if (!lock_ || !night_start_ms) {
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

    xSemaphoreTake(lock_, portMAX_DELAY);

    enqueue_total_++;
    last_enqueue_night_ms_ = night_start_ms;
    last_enqueue_therapy_index_ = therapy_index;
    copy_cstr(last_service_block_, sizeof(last_service_block_), "");

    for (size_t i = 0; i < count_; ++i) {
        ResultBuildJob &job = queue_[i];
        if (job.night_start_ms != night_start_ms) continue;

        job.therapy_index = therapy_index;
        bool upgraded = false;
        if (refresh && !job.refresh) {
            job.refresh = true;
            upgraded = true;
        }
        if (!idle_prebuild && job.idle_prebuild) {
            job.idle_prebuild = false;
            upgraded = true;
        }
        if (upgraded) {
            job.next_attempt_ms = 0;
            job.retry_attempts = 0;
            job.defer_count = 0;
            job.token = next_token_locked();
        }

        already_total_++;
        copy_cstr(last_enqueue_result_, sizeof(last_enqueue_result_), "already");
        const size_t count = count_;

        xSemaphoreGive(lock_);

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

    if (count_ < AC_REPORT_BUILD_QUEUE_MAX) {
        ResultBuildJob &job = queue_[count_];
        job = ResultBuildJob{};
        job.night_start_ms = night_start_ms;
        job.therapy_index = therapy_index;
        job.refresh = refresh;
        job.idle_prebuild = idle_prebuild;
        job.queued_ms = millis();
        job.token = next_token_locked();

        count_++;
        queued_total_++;
        copy_cstr(last_enqueue_result_, sizeof(last_enqueue_result_), "queued");
        const size_t count = count_;

        xSemaphoreGive(lock_);

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

    copy_cstr(last_enqueue_result_, sizeof(last_enqueue_result_), "full");
    xSemaphoreGive(lock_);

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

bool ReportBuildQueue::has_capacity() const {
    if (!lock_) return false;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return false;

    const bool available = count_ < AC_REPORT_BUILD_QUEUE_MAX;

    xSemaphoreGive(lock_);
    return available;
}

bool ReportBuildQueue::has_pending() const {
    if (!lock_) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool pending = count_ > 0;
    xSemaphoreGive(lock_);

    return pending;
}

bool ReportBuildQueue::has_foreground_pending() const {
    if (!lock_) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);

    bool pending = false;
    for (size_t i = 0; i < count_; ++i) {
        if (!queue_[i].idle_prebuild) {
            pending = true;
            break;
        }
    }

    xSemaphoreGive(lock_);
    return pending;
}

void ReportBuildQueue::clear(uint64_t night_start_ms, bool all) {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    ResultBuildJob kept[AC_REPORT_BUILD_QUEUE_MAX];
    size_t kept_count = 0;
    for (size_t i = 0; i < count_; ++i) {
        const ResultBuildJob &job = queue_[i];
        if (!all && job.night_start_ms != night_start_ms) {
            kept[kept_count++] = job;
        }
    }

    for (size_t i = 0; i < AC_REPORT_BUILD_QUEUE_MAX; ++i) {
        queue_[i] = i < kept_count ? kept[i] : ResultBuildJob{};
    }

    count_ = kept_count;

    xSemaphoreGive(lock_);
}

void ReportBuildQueue::note_read(const char *state) {
    if (!lock_) return;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return;

    copy_cstr(last_read_, sizeof(last_read_), state ? state : "");

    xSemaphoreGive(lock_);
}

void ReportBuildQueue::note_service_block(const char *reason) {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);
    if (count_ > 0) {
        copy_cstr(last_service_block_,
                  sizeof(last_service_block_),
                  reason ? reason : "");
    }
    xSemaphoreGive(lock_);
}

ReportBuildQueue::BuildQueueSelection ReportBuildQueue::select_next(
    uint32_t now_ms,
    ResultBuildJob &out) const {
    if (!lock_) return BuildQueueSelection::Empty;

    xSemaphoreTake(lock_, portMAX_DELAY);

    size_t selected_index = 0;
    const BuildQueueSelection selection =
        report_manager_internal::select_result_build_job(
            queue_, count_, now_ms, selected_index);
    out = selection == BuildQueueSelection::Ready
        ? queue_[selected_index]
        : ResultBuildJob{};

    xSemaphoreGive(lock_);
    return selection;
}

void ReportBuildQueue::note_service_started() {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    service_total_++;
    copy_cstr(last_service_block_, sizeof(last_service_block_), "");

    xSemaphoreGive(lock_);
}

void ReportBuildQueue::note_build_result(const ResultBuildJob &job,
                                         const char *outcome,
                                         const char *state,
                                         const char *error) {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);

    last_night_ms_ = job.night_start_ms;
    last_therapy_index_ = job.therapy_index;
    copy_cstr(last_outcome_, sizeof(last_outcome_), outcome ? outcome : "");
    copy_cstr(last_state_, sizeof(last_state_), state ? state : "");
    copy_cstr(last_error_, sizeof(last_error_), error ? error : "");

    xSemaphoreGive(lock_);
}

ReportBuildQueue::BuildQueueDeferResult ReportBuildQueue::defer(
    const ResultBuildJob &job,
    bool retry,
    uint32_t now_ms) {
    if (!lock_) return BuildQueueDeferResult::Stale;

    xSemaphoreTake(lock_, portMAX_DELAY);

    size_t index = 0;
    if (!find_job_locked(job, index)) {
        xSemaphoreGive(lock_);
        return BuildQueueDeferResult::Stale;
    }

    ResultBuildJob &queued = queue_[index];
    if (queued.defer_count < UINT16_MAX) queued.defer_count++;
    if (!retry) {
        queued.retry_attempts = 0;
        queued.next_attempt_ms = now_ms + AC_BG_WORKER_BUSY_RECHECK_MS;
        if (queued.next_attempt_ms == 0) queued.next_attempt_ms = 1;
        xSemaphoreGive(lock_);
        return BuildQueueDeferResult::Deferred;
    }

    if (queued.retry_attempts >= AC_REPORT_BUILD_RETRY_MAX) {
        erase_locked(index);
        xSemaphoreGive(lock_);
        return BuildQueueDeferResult::RetryExhausted;
    }

    queued.retry_attempts++;
    queued.next_attempt_ms =
        now_ms + report_manager_internal::result_build_retry_delay_ms(
                     queued.retry_attempts);
    if (queued.next_attempt_ms == 0) queued.next_attempt_ms = 1;

    xSemaphoreGive(lock_);
    return BuildQueueDeferResult::Deferred;
}

bool ReportBuildQueue::remove(const ResultBuildJob &job) {
    if (!lock_) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);

    size_t index = 0;
    const bool matched = find_job_locked(job, index);
    if (matched) erase_locked(index);

    xSemaphoreGive(lock_);
    return matched;
}

bool ReportBuildQueue::find_job_locked(const ResultBuildJob &job,
                                       size_t &index) const {
    for (size_t i = 0; i < count_; ++i) {
        if (queue_[i].night_start_ms != job.night_start_ms) continue;
        if (queue_[i].token != job.token) return false;

        index = i;
        return true;
    }

    return false;
}

void ReportBuildQueue::erase_locked(size_t index) {
    if (index >= count_) return;

    for (size_t i = index; i + 1 < count_; ++i) {
        queue_[i] = queue_[i + 1];
    }
    queue_[count_ - 1] = ResultBuildJob{};
    count_--;
}

uint32_t ReportBuildQueue::next_token_locked() {
    const uint32_t token = next_token_++;
    if (next_token_ == 0) next_token_ = 1;
    return token;
}

ReportBuildQueueService::ReportBuildQueueService(
    ReportBuildRuntime &build,
    ReportNightIndexService &night_index,
    ReportResultCacheRuntime &result_cache)
    : build_(build),
      night_index_(night_index),
      result_cache_(result_cache) {}

ReportBuildQueueSnapshot ReportBuildQueueService::snapshot() const {
    return build_.snapshot();
}

bool ReportBuildQueueService::has_pending() const {
    return build_.has_pending();
}

bool ReportBuildQueueService::has_foreground_pending() const {
    return build_.has_foreground_pending();
}

void ReportBuildQueueService::clear(uint64_t night_start_ms, bool all) {
    build_.clear(night_start_ms, all);
}

void ReportBuildQueueService::note_service_block(const char *reason) {
    build_.note_service_block(reason);
}

ReportBuildQueueService::BuildQueueSelection
ReportBuildQueueService::select_next(uint32_t now_ms,
                                     ResultBuildJob &out) const {
    return build_.select_next(now_ms, out);
}

void ReportBuildQueueService::note_service_started() {
    build_.note_service_started();
}

void ReportBuildQueueService::note_build_result(const ResultBuildJob &job,
                                                const char *outcome,
                                                const char *state,
                                                const char *error) {
    build_.note_build_result(job, outcome, state, error);
}

ReportBuildQueueService::BuildQueueDeferResult
ReportBuildQueueService::defer(const ResultBuildJob &job,
                               bool retry,
                               uint32_t now_ms) {
    return build_.defer(job, retry, now_ms);
}

bool ReportBuildQueueService::remove(const ResultBuildJob &job) {
    return build_.remove(job);
}

bool ReportBuildQueueService::request_prepare_by_therapy_index(
    size_t therapy_index,
    bool refresh_cache) {
    ScopedIndexedNight indexed_night("request_result_prepare_index");
    if (!indexed_night ||
        !night_index_.by_therapy_index(therapy_index, indexed_night.get())) {
        return false;
    }

    if (refresh_cache) {
        result_cache_.invalidate(indexed_night->summary.start_ms, false);
    }

    const ReportBuildRuntime::BuildQueueResult queued =
        build_.enqueue(indexed_night->summary.start_ms,
                       therapy_index,
                       refresh_cache,
                       false);
    return queued == ReportBuildRuntime::BuildQueueResult::Queued ||
           queued == ReportBuildRuntime::BuildQueueResult::AlreadyQueued;
}

bool ReportBuildQueueService::request_prepare_by_start(
    uint64_t night_start_ms,
    bool refresh_cache) {
    ScopedIndexedNight indexed_night("request_result_prepare_start_index");
    size_t therapy_index = 0;
    if (!indexed_night ||
        !night_index_.by_start(night_start_ms,
                               indexed_night.get(),
                               &therapy_index)) {
        return false;
    }

    if (refresh_cache) {
        result_cache_.invalidate(indexed_night->summary.start_ms, false);
    }

    const ReportBuildRuntime::BuildQueueResult queued =
        build_.enqueue(indexed_night->summary.start_ms,
                       therapy_index,
                       refresh_cache,
                       false);
    return queued == ReportBuildRuntime::BuildQueueResult::Queued ||
           queued == ReportBuildRuntime::BuildQueueResult::AlreadyQueued;
}

ReportManager::BuildQueueSnapshot ReportManager::build_queue_snapshot() const {
    return build_queue_service_.snapshot();
}

}  // namespace aircannect
