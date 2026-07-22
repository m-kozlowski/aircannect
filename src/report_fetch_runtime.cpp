#include "report_fetch_runtime.h"

namespace aircannect {

bool ReportFetchRuntime::start_summary_fetch(
    const SpoolClientRequest &request,
    uint32_t now_ms) {
    if (!spool_.begin(request)) return false;

    summary_.start(now_ms);
    return true;
}

void ReportFetchRuntime::finish_summary_fetch() {
    summary_.finish();
}

void ReportFetchRuntime::update_cache_active_source(ReportSourceId source) {
    cache_.update_active_source(source, spool_.status());
}

void ReportFetchRuntime::update_cache_spool() {
    cache_.update_spool(spool_.status());
}

void ReportFetchRuntime::finish_cache_fetch() {
    cache_.finish(spool_.status());
}

void ReportFetchRuntime::fail_cache_fetch(const char *message) {
    cache_.fail(message, spool_.status());
}

void ReportFetchRuntime::cancel_cache_fetch(const char *message) {
    cache_.cancel(message, spool_.status());
}

void ReportFetchRuntime::poll_spool(bool transport_backpressure_active,
                                    uint32_t rx_queue_full_alerts) {
    spool_.poll(transport_backpressure_active, rx_queue_full_alerts);
}

}  // namespace aircannect
