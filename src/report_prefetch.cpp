#include "report_manager.h"

#include <stdio.h>

#include "debug_log.h"
#include "report_sources.h"

namespace aircannect {
namespace {

const char *prefetch_phase_name(ReportManager::PrefetchPhase phase) {
    switch (phase) {
        case ReportManager::PrefetchPhase::Idle: return "idle";
        case ReportManager::PrefetchPhase::Selecting: return "selecting";
        case ReportManager::PrefetchPhase::Pending: return "pending";
        case ReportManager::PrefetchPhase::Fetching: return "fetching";
        case ReportManager::PrefetchPhase::Done: return "done";
        case ReportManager::PrefetchPhase::Failed: return "failed";
        case ReportManager::PrefetchPhase::Drained: return "drained";
    }
    return "unknown";
}

bool prefetch_deadline_before(uint32_t candidate,
                              uint32_t current,
                              uint32_t now_ms) {
    return static_cast<int32_t>(candidate - now_ms) <
           static_cast<int32_t>(current - now_ms);
}

}  // namespace

bool ReportManager::prefetch_in_cooldown(uint64_t night_ms,
                                         uint32_t now_ms) const {
    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;

    bool in_cooldown = false;
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (prefetch_skip_[i].night_ms == night_ms &&
            prefetch_skip_[i].until_ms != 0 &&
            static_cast<int32_t>(now_ms - prefetch_skip_[i].until_ms) < 0) {
            in_cooldown = true;
            break;
        }
    }

    if (locked) xSemaphoreGive(prefetch_lock_);
    return in_cooldown;
}

void ReportManager::prefetch_note_failure(uint64_t night_ms) {
    const uint32_t now_ms = millis();
    uint32_t until = now_ms + AC_REPORT_PREFETCH_FAIL_COOLDOWN_MS;
    if (until == 0) until = 1;

    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;

    size_t pick = 0;
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (prefetch_skip_[i].night_ms == night_ms ||
            prefetch_skip_[i].night_ms == 0) {
            pick = i;
            break;
        }
        if (prefetch_deadline_before(prefetch_skip_[i].until_ms,
                                     prefetch_skip_[pick].until_ms,
                                     now_ms)) {
            pick = i;
        }
    }

    prefetch_skip_[pick].night_ms = night_ms;
    prefetch_skip_[pick].until_ms = until;

    if (locked) xSemaphoreGive(prefetch_lock_);
}

void ReportManager::set_prefetch_phase(PrefetchPhase phase,
                                       uint64_t night_ms,
                                       bool inc_completed,
                                       bool inc_failed) {
    if (!prefetch_lock_) return;

    uint64_t failed_night = 0;
    uint32_t failed_total = 0;
    char failed_source[32] = {};
    char failed_error[64] = {};

    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const uint64_t phase_night =
        night_ms != 0 ? night_ms : prefetch_active_night_;

    prefetch_phase_ = phase;
    prefetch_active_night_ = night_ms;
    if (night_ms != 0) prefetch_last_night_ = night_ms;
    if (inc_completed) prefetch_completed_++;

    if (inc_failed) {
        prefetch_failed_++;
        prefetch_last_failed_night_ = phase_night;
        if (phase_night != 0) prefetch_last_night_ = phase_night;

        const char *source =
            cache_status_.source_count
                ? report_source_spool_type(cache_status_.active_source)
                : "";
        snprintf(prefetch_last_source_,
                 sizeof(prefetch_last_source_),
                 "%s",
                 source ? source : "");
        snprintf(prefetch_last_error_,
                 sizeof(prefetch_last_error_),
                 "%s",
                 cache_status_.error.length() ? cache_status_.error.c_str()
                                              : "");

        failed_night = phase_night;
        failed_total = prefetch_failed_;
        snprintf(failed_source,
                 sizeof(failed_source),
                 "%s",
                 prefetch_last_source_);
        snprintf(failed_error,
                 sizeof(failed_error),
                 "%s",
                 prefetch_last_error_);
    }
    xSemaphoreGive(prefetch_lock_);

    if (inc_failed) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "prefetch failed phase=%s night=%llu source=%s "
                  "error=%s total=%lu\n",
                  prefetch_phase_name(phase),
                  static_cast<unsigned long long>(failed_night),
                  failed_source[0] ? failed_source : "--",
                  failed_error[0] ? failed_error : "--",
                  static_cast<unsigned long>(failed_total));
    }
}

bool ReportManager::prefetch_request_night(uint64_t night_start_ms) {
    if (!prefetch_lock_ || night_start_ms == 0) return false;

    bool accepted = false;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    if (prefetch_phase_ != PrefetchPhase::Pending &&
        prefetch_phase_ != PrefetchPhase::Selecting &&
        prefetch_phase_ != PrefetchPhase::Fetching) {
        prefetch_phase_ = PrefetchPhase::Pending;
        prefetch_active_night_ = night_start_ms;
        prefetch_last_night_ = night_start_ms;
        accepted = true;
    }
    xSemaphoreGive(prefetch_lock_);

    return accepted;
}

bool ReportManager::prefetch_request_candidate() {
    if (!prefetch_lock_) return false;

    bool accepted = false;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    if (prefetch_phase_ != PrefetchPhase::Selecting &&
        prefetch_phase_ != PrefetchPhase::Pending &&
        prefetch_phase_ != PrefetchPhase::Fetching) {
        prefetch_phase_ = PrefetchPhase::Selecting;
        prefetch_active_night_ = 0;
        accepted = true;
    }
    xSemaphoreGive(prefetch_lock_);

    return accepted;
}

void ReportManager::prefetch_mark_drained() {
    if (!prefetch_lock_) return;

    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    if (prefetch_phase_ != PrefetchPhase::Pending &&
        prefetch_phase_ != PrefetchPhase::Selecting &&
        prefetch_phase_ != PrefetchPhase::Fetching) {
        prefetch_phase_ = PrefetchPhase::Drained;
        prefetch_active_night_ = 0;
    }
    xSemaphoreGive(prefetch_lock_);
}

void ReportManager::prefetch_preempt() {
    if (!prefetch_lock_) return;

    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    prefetch_preempt_req_ = true;
    xSemaphoreGive(prefetch_lock_);
}

ReportManager::PrefetchSnapshot ReportManager::prefetch_snapshot() const {
    PrefetchSnapshot snap;
    if (!prefetch_lock_) return snap;

    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    snap.phase = prefetch_phase_;
    snap.night_ms = prefetch_active_night_;
    snap.last_night_ms = prefetch_last_night_;
    snap.last_failed_night_ms = prefetch_last_failed_night_;
    snap.completed = prefetch_completed_;
    snap.failed = prefetch_failed_;
    snprintf(snap.last_source,
             sizeof(snap.last_source),
             "%s",
             prefetch_last_source_);
    snprintf(snap.last_error,
             sizeof(snap.last_error),
             "%s",
             prefetch_last_error_);
    xSemaphoreGive(prefetch_lock_);

    return snap;
}

void ReportManager::prefetch_yield_to_foreground() {
    if (!prefetch_lock_) return;

    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const bool owns = (prefetch_phase_ == PrefetchPhase::Fetching);
    xSemaphoreGive(prefetch_lock_);
    if (!owns) return;

    if (cache_fetch_active_) {
        spool_.reset();
        abort_cache_write_fetch();
        cache_fetch_active_ = false;
        cache_status_.active = false;
        cache_status_.revision++;
        cache_status_.error = "preempted_by_user";
    }

    set_prefetch_phase(PrefetchPhase::Idle, 0, false, false);
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "prefetch yielded to foreground prepare\n");
}

void ReportManager::service_prefetch(bool realtime_active) {
    if (!prefetch_lock_) return;

    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const PrefetchPhase phase = prefetch_phase_;
    const bool preempt = prefetch_preempt_req_;
    prefetch_preempt_req_ = false;
    const uint64_t active = prefetch_active_night_;
    xSemaphoreGive(prefetch_lock_);

    if (preempt && (phase == PrefetchPhase::Selecting ||
                    phase == PrefetchPhase::Fetching ||
                    phase == PrefetchPhase::Pending)) {
        if (cache_fetch_active_) {
            spool_.reset();
            abort_cache_write_fetch();
            cache_fetch_active_ = false;
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error = "prefetch_preempted";
        }
        set_prefetch_phase(PrefetchPhase::Idle, 0, false, false);
        return;
    }

    if (phase == PrefetchPhase::Selecting) {
        if (realtime_active || busy()) return;
        if (edf_report_catalog_pending()) return;

        uint64_t night = 0;
        if (next_night_needing_cache(night) && night != 0) {
            set_prefetch_phase(PrefetchPhase::Pending, night, false, false);
        } else {
            set_prefetch_phase(PrefetchPhase::Drained, 0, false, false);
        }
        return;
    }

    if (phase == PrefetchPhase::Fetching && !cache_fetch_active_) {
        ReportNightCoverageStatus coverage;
        const bool covered =
            night_coverage(active, coverage) && coverage.missing_required == 0;

        if (!covered) prefetch_note_failure(active);
        set_prefetch_phase(covered ? PrefetchPhase::Done : PrefetchPhase::Failed,
                           active,
                           covered,
                           !covered);
        return;
    }

    if (realtime_active) {
        if (phase == PrefetchPhase::Fetching && cache_fetch_active_) {
            spool_.reset();
            abort_cache_write_fetch();
            cache_fetch_active_ = false;
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error = "preempted_by_stream";
            set_prefetch_phase(PrefetchPhase::Idle, 0, false, false);
            Log::logf(CAT_REPORT, LOG_DEBUG,
                      "prefetch yielded to stream activity\n");
        }
        return;
    }

    if (phase == PrefetchPhase::Pending && !busy()) {
        if (active != 0 && request_night_cache(active, false)) {
            set_prefetch_phase(PrefetchPhase::Fetching, active, false, false);
        } else if (active != 0) {
            set_prefetch_phase(PrefetchPhase::Failed, active, false, true);
        } else {
            set_prefetch_phase(PrefetchPhase::Drained, 0, false, false);
        }
    }
}

}  // namespace aircannect
