#include "report_plot_prebuild_state.h"

namespace aircannect {

bool ReportPlotPrebuildState::key_matches(
    const ReportNightIndexCacheKey &key) const {
    return key_valid_ && report_night_index_cache_key_equal(key_, key);
}

void ReportPlotPrebuildState::reset_for_key(
    const ReportNightIndexCacheKey &key) {
    key_valid_ = true;
    key_ = key;
    cursor_ = 0;
    next_scan_ms_ = 0;
}

bool ReportPlotPrebuildState::rescan_delay_active(uint32_t now_ms) const {
    return next_scan_ms_ != 0 &&
           static_cast<int32_t>(now_ms - next_scan_ms_) < 0;
}

void ReportPlotPrebuildState::mark_drained(uint32_t now_ms,
                                           uint32_t delay_ms) {
    next_scan_ms_ = now_ms + delay_ms;
    if (next_scan_ms_ == 0) next_scan_ms_ = 1;
}

void ReportPlotPrebuildState::rewind_cursor() {
    if (cursor_ > 0) cursor_--;
}

}  // namespace aircannect
