#include "report_prefetch_state.h"

#include <Arduino.h>
#include <stdio.h>

#include "debug_log.h"

namespace aircannect {
namespace {

bool prefetch_deadline_before(uint32_t candidate,
                              uint32_t current,
                              uint32_t now_ms) {
    return static_cast<int32_t>(candidate - now_ms) <
           static_cast<int32_t>(current - now_ms);
}

}  // namespace

const char *report_prefetch_phase_name(ReportPrefetchPhase phase) {
    switch (phase) {
        case ReportPrefetchPhase::Idle: return "idle";
        case ReportPrefetchPhase::Selecting: return "selecting";
        case ReportPrefetchPhase::Pending: return "pending";
        case ReportPrefetchPhase::Fetching: return "fetching";
        case ReportPrefetchPhase::Done: return "done";
        case ReportPrefetchPhase::Failed: return "failed";
        case ReportPrefetchPhase::Drained: return "drained";
    }

    return "unknown";
}

ReportPrefetchState::~ReportPrefetchState() {
    if (!lock_) return;

    vSemaphoreDelete(lock_);
    lock_ = nullptr;
}

bool ReportPrefetchState::begin() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

bool ReportPrefetchState::in_cooldown(uint64_t night_ms,
                                      uint32_t now_ms) const {
    if (!lock_) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);

    bool in_cooldown = false;
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (skip_[i].night_ms == night_ms &&
            skip_[i].until_ms != 0 &&
            static_cast<int32_t>(now_ms - skip_[i].until_ms) < 0) {
            in_cooldown = true;
            break;
        }
    }

    xSemaphoreGive(lock_);
    return in_cooldown;
}

void ReportPrefetchState::note_failure(uint64_t night_ms) {
    if (!lock_) return;

    const uint32_t now_ms = millis();
    uint32_t until = now_ms + AC_REPORT_PREFETCH_FAIL_COOLDOWN_MS;
    if (until == 0) until = 1;

    xSemaphoreTake(lock_, portMAX_DELAY);

    size_t pick = 0;
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (skip_[i].night_ms == night_ms || skip_[i].night_ms == 0) {
            pick = i;
            break;
        }
        if (prefetch_deadline_before(skip_[i].until_ms,
                                     skip_[pick].until_ms,
                                     now_ms)) {
            pick = i;
        }
    }

    skip_[pick].night_ms = night_ms;
    skip_[pick].until_ms = until;

    xSemaphoreGive(lock_);
}

void ReportPrefetchState::set_phase(ReportPrefetchPhase phase,
                                    uint64_t night_ms,
                                    bool inc_completed,
                                    bool inc_failed,
                                    const char *failed_source,
                                    const char *failed_error) {
    if (!lock_) return;

    uint64_t failed_night = 0;
    uint32_t failed_total = 0;
    char source_copy[sizeof(last_source_)] = {};
    char error_copy[sizeof(last_error_)] = {};

    xSemaphoreTake(lock_, portMAX_DELAY);

    const uint64_t phase_night = night_ms != 0 ? night_ms : active_night_;

    phase_ = phase;
    active_night_ = night_ms;
    if (night_ms != 0) last_night_ = night_ms;
    if (inc_completed) completed_++;

    if (inc_failed) {
        failed_++;
        last_failed_night_ = phase_night;
        if (phase_night != 0) last_night_ = phase_night;

        snprintf(last_source_,
                 sizeof(last_source_),
                 "%s",
                 failed_source ? failed_source : "");
        snprintf(last_error_,
                 sizeof(last_error_),
                 "%s",
                 failed_error ? failed_error : "");

        failed_night = phase_night;
        failed_total = failed_;
        snprintf(source_copy, sizeof(source_copy), "%s", last_source_);
        snprintf(error_copy, sizeof(error_copy), "%s", last_error_);
    }

    xSemaphoreGive(lock_);

    if (!inc_failed) return;

    Log::logf(CAT_REPORT,
              LOG_WARN,
              "prefetch failed phase=%s night=%llu source=%s "
              "error=%s total=%lu\n",
              report_prefetch_phase_name(phase),
              static_cast<unsigned long long>(failed_night),
              source_copy[0] ? source_copy : "--",
              error_copy[0] ? error_copy : "--",
              static_cast<unsigned long>(failed_total));
}

bool ReportPrefetchState::request_candidate() {
    if (!lock_) return false;

    bool accepted = false;
    xSemaphoreTake(lock_, portMAX_DELAY);

    if (phase_ != ReportPrefetchPhase::Selecting &&
        phase_ != ReportPrefetchPhase::Pending &&
        phase_ != ReportPrefetchPhase::Fetching) {
        phase_ = ReportPrefetchPhase::Selecting;
        active_night_ = 0;
        accepted = true;
    }

    xSemaphoreGive(lock_);
    return accepted;
}

void ReportPrefetchState::preempt() {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);
    preempt_req_ = true;
    xSemaphoreGive(lock_);
}

bool ReportPrefetchState::is_fetching() const {
    if (!lock_) return false;

    xSemaphoreTake(lock_, portMAX_DELAY);
    const bool fetching = phase_ == ReportPrefetchPhase::Fetching;
    xSemaphoreGive(lock_);

    return fetching;
}

ReportPrefetchSnapshot ReportPrefetchState::snapshot() const {
    ReportPrefetchSnapshot snap;
    if (!lock_) return snap;

    xSemaphoreTake(lock_, portMAX_DELAY);

    snap.phase = phase_;
    snap.night_ms = active_night_;
    snap.last_night_ms = last_night_;
    snap.last_failed_night_ms = last_failed_night_;
    snap.completed = completed_;
    snap.failed = failed_;
    snprintf(snap.last_source, sizeof(snap.last_source), "%s", last_source_);
    snprintf(snap.last_error, sizeof(snap.last_error), "%s", last_error_);

    xSemaphoreGive(lock_);
    return snap;
}

ReportPrefetchServiceState ReportPrefetchState::take_service_state() {
    ReportPrefetchServiceState state;
    if (!lock_) return state;

    xSemaphoreTake(lock_, portMAX_DELAY);

    state.phase = phase_;
    state.preempt = preempt_req_;
    state.active_night_ms = active_night_;
    preempt_req_ = false;

    xSemaphoreGive(lock_);
    return state;
}

}  // namespace aircannect
