#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_cache_fetch_state.h"
#include "report_pending_result_prepare.h"
#include "report_spool_runtime.h"
#include "report_summary_fetch_state.h"

namespace aircannect {

class ReportFetchRuntime {
public:
    explicit ReportFetchRuntime(RpcRequestPort &rpc) : spool_(rpc) {}

    bool start_summary_fetch(const SpoolClientRequest &request,
                             uint32_t now_ms);
    void finish_summary_fetch();

    bool summary_active() const { return summary_.active(); }
    uint32_t summary_elapsed_ms(uint32_t now_ms) const {
        return summary_.elapsed_ms(now_ms);
    }

    bool cache_active() const { return cache_.active(); }
    bool any_fetch_active() const {
        return summary_.active() || cache_.active();
    }

    ReportCacheFetchState &cache() { return cache_; }
    const ReportCacheFetchState &cache() const { return cache_; }
    const ReportCacheFetchStatus &cache_status() const {
        return cache_.status();
    }

    void update_cache_active_source(ReportSourceId source);
    void update_cache_spool();
    void finish_cache_fetch();
    void fail_cache_fetch(const char *message);
    void cancel_cache_fetch(const char *message);

    void set_pending_prepare(size_t therapy_index, bool refresh_cache) {
        pending_.set(therapy_index, refresh_cache);
    }
    bool take_pending_prepare(ReportPendingResultPrepare &out) {
        return pending_.take(out);
    }

    bool begin_spool(const SpoolClientRequest &request) {
        return spool_.begin(request);
    }
    void reset_spool() { spool_.reset(); }
    void poll_spool(bool transport_backpressure_active,
                    uint32_t rx_queue_full_alerts);
    bool enqueue_spool_notification(const char *payload, size_t payload_len) {
        return spool_.enqueue_notification(payload, payload_len);
    }
    bool drain_spool_notification() {
        return spool_.drain_notification();
    }
    bool spool_complete() const { return spool_.complete(); }
    bool spool_failed() const { return spool_.failed(); }
    SpoolClientStatus spool_status() const { return spool_.status(); }
    bool take_completed_round(ReportSpoolResult &out) {
        return spool_.take_completed_round(out);
    }
    void move_spool_result_to(ReportSpoolResult &out) {
        spool_.move_result_to(out);
    }

    void observe_idle(uint32_t rx_queue_full_alerts) {
        spool_.observe_idle(rx_queue_full_alerts);
    }

private:
    ReportSpoolRuntime spool_;
    ReportSummaryFetchState summary_;
    ReportCacheFetchState cache_;
    ReportPendingResultPrepareState pending_;
};

}  // namespace aircannect
