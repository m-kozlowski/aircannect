#pragma once

#include <stddef.h>

#include "board_report.h"
#include "fixed_queue.h"
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

    bool enqueue_notification(const RpcPayloadRef &payload);
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

    SpoolClient client_;
    ReportSpoolPressureMonitor pressure_;
    FixedQueue<RpcPayloadRef, AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH>
        notifications_;
    bool notification_loss_pending_ = false;
};

}  // namespace aircannect
