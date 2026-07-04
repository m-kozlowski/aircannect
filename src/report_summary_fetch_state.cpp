#include "report_summary_fetch_state.h"

namespace aircannect {

void ReportSummaryFetchState::start(uint32_t now_ms) {
    active_ = true;
    started_ms_ = now_ms;
}

void ReportSummaryFetchState::finish() {
    active_ = false;
    started_ms_ = 0;
}

uint32_t ReportSummaryFetchState::elapsed_ms(uint32_t now_ms) const {
    return started_ms_ ? now_ms - started_ms_ : 0;
}

}  // namespace aircannect
