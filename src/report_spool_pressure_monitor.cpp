#include "report_spool_pressure_monitor.h"

#include "debug_log.h"
#include "rpc_arbiter.h"
#include "spool_client.h"

namespace aircannect {

void ReportSpoolPressureMonitor::observe_idle(const RpcArbiter &arbiter) {
    observed_rx_queue_full_alerts_ =
        arbiter.can_driver().stats().rx_queue_full_alerts;
}

void ReportSpoolPressureMonitor::log_if_changed(const RpcArbiter &arbiter,
                                                const SpoolClient &spool) {
    const uint32_t alerts =
        arbiter.can_driver().stats().rx_queue_full_alerts;
    if (alerts == observed_rx_queue_full_alerts_) return;

    observed_rx_queue_full_alerts_ = alerts;
    if (!spool.active()) return;

    const SpoolClientStatus &status = spool.status();
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "spool CAN RX pressure source=%s state=%s round=%u "
              "spool_id=%lu round_fragments=%lu round_bytes=%lu "
              "total_fragments=%lu total_bytes=%lu alerts=%lu\n",
              status.spool_type.c_str(),
              spool_client_state_name(status.state),
              static_cast<unsigned>(status.current_round),
              static_cast<unsigned long>(status.active_spool_id),
              static_cast<unsigned long>(status.round_fragments),
              static_cast<unsigned long>(status.round_bytes),
              static_cast<unsigned long>(status.fragments),
              static_cast<unsigned long>(status.bytes),
              static_cast<unsigned long>(alerts));
}

}  // namespace aircannect
