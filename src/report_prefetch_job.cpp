#include "report_prefetch_job.h"

#include <Arduino.h>

#include "board.h"
#include "report_manager.h"

namespace aircannect {

JobStep ReportPrefetchJob::step() {
    const ReportManager::PrefetchSnapshot snap = report_->prefetch_snapshot();

    switch (state_) {
    case State::Pick: {
        // A summary refresh (e.g. right after therapy stops) may include a new night,
        // rescan immediately rather than waiting out the backoff.
        const uint32_t summary_rev = report_->summary_revision();
        if (summary_rev != last_summary_rev_) {
            last_summary_rev_ = summary_rev;
            rescan_after_ms_ = 0;
        }
        // After the queue drained, back off before re-scanning coverage so we
        // don't hit the card every tick when there is nothing to fetch.
        if (rescan_after_ms_ != 0 &&
            static_cast<int32_t>(millis() - rescan_after_ms_) < 0) {
            return JobStep::Idle;
        }
        rescan_after_ms_ = 0;
        if (!report_->prefetch_request_next()) {
            return JobStep::Waiting;  // a request is already in flight
        }
        state_ = State::Wait;
        return JobStep::Waiting;
    }

    case State::Wait:
        if (snap.phase == ReportManager::PrefetchPhase::Pending ||
            snap.phase == ReportManager::PrefetchPhase::Fetching) {
            return JobStep::Waiting;  // main loop is spooling it
        }
        state_ = State::Pick;
        if (snap.phase == ReportManager::PrefetchPhase::Done) {
            consec_fail_ = 0;
            return JobStep::Working;  // try the next night now
        }
        if (snap.phase == ReportManager::PrefetchPhase::Failed) {
            if (++consec_fail_ >= AC_REPORT_PREFETCH_FAIL_BURST) {
                consec_fail_ = AC_REPORT_PREFETCH_FAIL_BURST;  // cap
                // Likely offline (every spool times out)
                rescan_after_ms_ = millis() + AC_REPORT_PREFETCH_OFFLINE_BACKOFF_MS;
                if (rescan_after_ms_ == 0) rescan_after_ms_ = 1;
                return JobStep::Idle;
            }
            return JobStep::Working;  // transient; try the next night
        }
        // Drained: nothing left to fetch right now.
        rescan_after_ms_ = millis() + AC_REPORT_PREFETCH_RESCAN_MS;
        if (rescan_after_ms_ == 0) rescan_after_ms_ = 1;
        return JobStep::Idle;
    }
    return JobStep::Idle;
}

void ReportPrefetchJob::on_preempt() {
    // Foreground needs the bus; abort any in-flight prefetch and re-pick later.
    if (state_ == State::Wait) {
        report_->prefetch_cancel();
        state_ = State::Pick;
    }
}

}  // namespace aircannect
