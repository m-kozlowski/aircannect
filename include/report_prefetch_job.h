#pragma once

#include <stdint.h>

#include "background_worker.h"

namespace aircannect {

class ReportManager;

// Background job: backfill nights whose coverage is incomplete, newest-first.
// It owns no CAN/SD state, only drives the ReportManager prefetch mailbox
// (the main loop runs the actual spool) and waits for each fetch to conclude.
class ReportPrefetchJob : public BackgroundJob {
public:
    explicit ReportPrefetchJob(ReportManager &report) : report_(&report) {}
    const char *name() const override { return "prefetch"; }
    JobStep step() override;
    void on_preempt() override;

private:
    enum class State { Pick, Wait };
    ReportManager *report_ = nullptr;
    State state_ = State::Pick;
    uint32_t rescan_after_ms_ = 0;  // don't Pick again until this time
    uint32_t consec_fail_ = 0;      // consecutive failures -> circuit breaker
};

}  // namespace aircannect
