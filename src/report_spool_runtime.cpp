#include "report_spool_runtime.h"

#include "debug_log.h"

namespace aircannect {

bool ReportSpoolRuntime::begin(const SpoolClientRequest &request) {
    clear_notifications();
    return client_.begin(request);
}

void ReportSpoolRuntime::reset() {
    clear_notifications();
    client_.reset();
}

void ReportSpoolRuntime::poll(bool transport_backpressure_active,
                              uint32_t rx_queue_full_alerts) {
    client_.poll(transport_backpressure_active ||
                 notification_backpressure_active());
    pressure_.log_if_changed(rx_queue_full_alerts, client_);
}

bool ReportSpoolRuntime::enqueue_notification(const RpcPayloadRef &payload) {
    if (!client_.active() || !payload) return false;
    if (notifications_.full()) {
        notification_loss_pending_ = true;
        return false;
    }

    if (!notifications_.push(payload)) {
        notification_loss_pending_ = true;
        return false;
    }

    return true;
}

bool ReportSpoolRuntime::drain_notification() {
    if (notification_loss_pending_) {
        notification_loss_pending_ = false;
        clear_notifications();
        Log::logf(CAT_REPORT, LOG_WARN,
                  "spool notification queue lost data; retrying round\n");
        client_.note_notification_loss("notification_queue_full");
        return true;
    }
    RpcPayloadRef notification;
    if (!notifications_.pop(notification)) return false;

    (void)client_.handle_spool_notification(
        rpc_payload_view(notification));
    return true;
}

void ReportSpoolRuntime::observe_idle(uint32_t rx_queue_full_alerts) {
    if (!client_.active()) pressure_.observe_idle(rx_queue_full_alerts);
}

bool ReportSpoolRuntime::notification_backpressure_active() const {
    return notifications_.count() >=
        AC_REPORT_SPOOL_NOTIFICATION_BACKPRESSURE_WATERMARK;
}

void ReportSpoolRuntime::clear_notifications() {
    notifications_.clear();
    notification_loss_pending_ = false;
}

}  // namespace aircannect
