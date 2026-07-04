#include "report_prefetch_runtime.h"

namespace aircannect {

bool ReportPrefetchRuntime::begin() {
    return prefetch_.begin();
}

bool ReportPrefetchRuntime::in_cooldown(uint64_t night_ms,
                                        uint32_t now_ms) const {
    return prefetch_.in_cooldown(night_ms, now_ms);
}

void ReportPrefetchRuntime::note_failure(uint64_t night_ms) {
    prefetch_.note_failure(night_ms);
}

void ReportPrefetchRuntime::set_phase(ReportPrefetchPhase phase,
                                      uint64_t night_ms,
                                      bool inc_completed,
                                      bool inc_failed,
                                      const char *failed_source,
                                      const char *failed_error) {
    prefetch_.set_phase(phase,
                        night_ms,
                        inc_completed,
                        inc_failed,
                        failed_source,
                        failed_error);
}

bool ReportPrefetchRuntime::request_candidate() {
    return prefetch_.request_candidate();
}

void ReportPrefetchRuntime::preempt() {
    prefetch_.preempt();
}

bool ReportPrefetchRuntime::is_fetching() const {
    return prefetch_.is_fetching();
}

ReportPrefetchSnapshot ReportPrefetchRuntime::snapshot() const {
    return prefetch_.snapshot();
}

ReportPrefetchServiceState ReportPrefetchRuntime::take_service_state() {
    return prefetch_.take_service_state();
}

}  // namespace aircannect
