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

bool ReportSpoolRuntime::enqueue_notification(const char *payload,
                                              size_t payload_len) {
    if (!client_.active() || !payload || payload_len == 0) return false;
    if (notification_count_ >= AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH) {
        notification_loss_pending_ = true;
        return false;
    }

    const size_t tail = (notification_head_ + notification_count_) %
                        AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH;
    LargeTextBuffer &notification = notifications_[tail];
    if (!notification.append(payload, payload_len)) {
        release_notification(tail);
        notification_loss_pending_ = true;
        return false;
    }

    notification_count_++;
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
    if (notification_count_ == 0) return false;

    LargeTextBuffer &notification = notifications_[notification_head_];
    (void)client_.handle_spool_notification(notification.c_str(),
                                            notification.length());
    release_notification(notification_head_);
    notification_head_ = (notification_head_ + 1) %
                         AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH;
    notification_count_--;
    return true;
}

void ReportSpoolRuntime::observe_idle(uint32_t rx_queue_full_alerts) {
    if (!client_.active()) pressure_.observe_idle(rx_queue_full_alerts);
}

bool ReportSpoolRuntime::notification_backpressure_active() const {
    return notification_count_ >=
        AC_REPORT_SPOOL_NOTIFICATION_BACKPRESSURE_WATERMARK;
}

void ReportSpoolRuntime::clear_notifications() {
    for (size_t i = 0; i < AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH; ++i) {
        release_notification(i);
    }
    notification_head_ = 0;
    notification_count_ = 0;
    notification_loss_pending_ = false;
}

void ReportSpoolRuntime::release_notification(size_t index) {
    LargeTextBuffer empty;
    notifications_[index].swap(empty);
}

}  // namespace aircannect
