#include "report_summary_snapshot.h"

namespace aircannect {

void ReportSummarySnapshot::request_publish() {
    requested_generation_++;
    if (requested_generation_ == published_generation_) {
        requested_generation_++;
    }
}

void ReportSummarySnapshot::publish(LargeTextBuffer &build_buffer) {
    snapshot_.swap(build_buffer);
    published_generation_ = requested_generation_;
}

void ReportSummarySnapshot::publish_fallback(LargeTextBuffer &build_buffer) {
    snapshot_.swap(build_buffer);
}

void ReportSummarySnapshot::build_json(LargeTextBuffer &json) const {
    if (!snapshot_.length()) {
        json = "{\"state\":\"idle\",\"error\":\"summary_snapshot_missing\","
               "\"nights\":[]}";
        return;
    }

    json.clear();
    json.append(snapshot_.c_str(), snapshot_.length());
}

void ReportSummarySnapshot::begin_progress(uint32_t now_ms,
                                           uint32_t interval_ms) {
    next_progress_ms_ = now_ms + interval_ms;
}

bool ReportSummarySnapshot::progress_due(uint32_t now_ms,
                                         uint32_t interval_ms) {
    if (static_cast<int32_t>(now_ms - next_progress_ms_) < 0) {
        return false;
    }

    next_progress_ms_ = now_ms + interval_ms;
    return true;
}

}  // namespace aircannect
