#pragma once

#include <stddef.h>

#include "board_report.h"
#include "large_text_buffer.h"
#include "report_spool_pressure_monitor.h"
#include "report_spool_types.h"
#include "rpc_request_port.h"
#include "spool_client.h"

namespace aircannect {

class ReportSpoolRuntime {
public:
    explicit ReportSpoolRuntime(RpcRequestPort &rpc) : client_(rpc) {}

    bool begin(const SpoolClientRequest &request);
    void reset();
    void poll(bool transport_backpressure_active,
              uint32_t rx_queue_full_alerts);

    bool enqueue_notification(const char *payload, size_t payload_len);
    bool drain_notification();

    bool active() const { return client_.active(); }
    bool complete() const { return client_.complete(); }
    bool failed() const { return client_.failed(); }
    SpoolClientStatus status() const { return client_.status(); }

    bool take_completed_round(ReportSpoolResult &out) {
        return client_.take_completed_round(out);
    }

    void move_result_to(ReportSpoolResult &out) {
        client_.move_result_to(out);
    }

    void observe_idle(uint32_t rx_queue_full_alerts);

private:
    bool notification_backpressure_active() const;
    void clear_notifications();
    void release_notification(size_t index);

    SpoolClient client_;
    ReportSpoolPressureMonitor pressure_;
    LargeTextBuffer notifications_[AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH];
    size_t notification_head_ = 0;
    size_t notification_count_ = 0;
    bool notification_loss_pending_ = false;
};

}  // namespace aircannect
