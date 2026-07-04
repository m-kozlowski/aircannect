#include "report_manager.h"

#include "debug_log.h"

namespace aircannect {
namespace {

const char *const REPORT_SUMMARY_FROM = "2000-01-01T00:00:00.000Z";

}  // namespace

bool ReportManager::request_summary_refresh(bool force) {
    if (summary_fetch_active_ && !force) return true;
    if (cache_fetch_active_) return false;

    SpoolClientRequest request;
    request.spool_type = "Summary";
    request.from_dt = REPORT_SUMMARY_FROM;
    request.max_size = AC_REPORT_SUMMARY_SPOOL_ROUND_BYTES;
    request.fragment_max = AC_REPORT_SPOOL_FRAGMENT_MAX_BYTES;
    request.max_notifications = AC_REPORT_SPOOL_MAX_NOTIFICATIONS_PER_PULL;
    request.max_rounds = 64;
    request.pace_on_backpressure = true;
    request.stream_rounds = false;

    if (!spool_.begin(request)) {
        fail_summary("summary_start_failed");
        return false;
    }

    summary_fetch_active_ = true;
    summary_started_ms_ = millis();
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Fetching;
        summary_status_.active_spool = "Summary";
        summary_status_.error.clear();
        summary_status_.spool = spool_.status();
        give_summary_lock();
    }
    publish_summary_json_snapshot();
    Log::logf(CAT_REPORT, LOG_INFO, "Summary refresh queued\n");
    return true;
}

}  // namespace aircannect
