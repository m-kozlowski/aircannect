#pragma once

#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board_report.h"
#include "report_manager_internal_types.h"
#include "report_manager_limits.h"

namespace aircannect {

enum class ReportPrefetchPhase : uint8_t {
    Idle,      // nothing requested
    Selecting, // worker asked; main loop chooses the next night
    Pending,   // worker asked; main loop starts it when not busy
    Fetching,  // main loop is spooling a night
    Done,      // last fetch fully covered its night
    Failed,    // last fetch ended incomplete / was preempted
    Drained,   // nothing left to prefetch
};

struct ReportPrefetchSnapshot {
    ReportPrefetchPhase phase = ReportPrefetchPhase::Idle;
    uint64_t night_ms = 0;
    uint64_t last_night_ms = 0;
    uint64_t last_failed_night_ms = 0;
    uint32_t completed = 0;
    uint32_t failed = 0;
    char last_source[48] = {};
    char last_error[48] = {};
};

struct ReportPrefetchServiceState {
    ReportPrefetchPhase phase = ReportPrefetchPhase::Idle;
    bool preempt = false;
    uint64_t active_night_ms = 0;
};

class ReportPrefetchState {
public:
    ~ReportPrefetchState();

    bool begin();

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
    using PrefetchSkip = report_manager_internal::PrefetchSkip;

    SemaphoreHandle_t lock_ = nullptr;
    ReportPrefetchPhase phase_ = ReportPrefetchPhase::Idle;
    uint64_t active_night_ = 0;
    uint64_t last_night_ = 0;
    uint64_t last_failed_night_ = 0;
    bool preempt_req_ = false;
    uint32_t completed_ = 0;
    uint32_t failed_ = 0;
    char last_source_[48] = {};
    char last_error_[48] = {};
    PrefetchSkip skip_[PREFETCH_SKIP_MAX] = {};
};

const char *report_prefetch_phase_name(ReportPrefetchPhase phase);

}  // namespace aircannect
