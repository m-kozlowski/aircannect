#include "report_pending_result_prepare.h"

namespace aircannect {

void ReportPendingResultPrepareState::set(size_t therapy_index,
                                          bool refresh_cache) {
    pending_.active = true;
    pending_.refresh_cache = refresh_cache;
    pending_.therapy_index = therapy_index;
}

bool ReportPendingResultPrepareState::take(ReportPendingResultPrepare &out) {
    if (!pending_.active) return false;

    out = pending_;
    clear();
    return true;
}

void ReportPendingResultPrepareState::clear() {
    pending_ = {};
}

}  // namespace aircannect
