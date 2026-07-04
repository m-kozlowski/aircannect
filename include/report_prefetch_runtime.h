#pragma once

#include <stdint.h>

#include "report_prefetch_state.h"

namespace aircannect {

class ReportPrefetchRuntime {
public:
    bool begin();

    // Prefetch state
    bool in_cooldown(uint64_t night_ms, uint32_t now_ms) const;
    void note_failure(uint64_t night_ms);
    void set_phase(ReportPrefetchPhase phase,
                   uint64_t night_ms,
                   bool inc_completed,
                   bool inc_failed,
                   const char *failed_source,
                   const char *failed_error);

    bool request_candidate();
    void preempt();
    bool is_fetching() const;

    ReportPrefetchSnapshot snapshot() const;
    ReportPrefetchServiceState take_service_state();

private:
    ReportPrefetchState prefetch_;
};

}  // namespace aircannect
