#pragma once

#include "report_spool_pressure_monitor.h"
#include "report_spool_types.h"
#include "rpc_arbiter.h"
#include "spool_client.h"

namespace aircannect {

class ReportSpoolRuntime {
public:
    bool begin(const SpoolClientRequest &request) {
        return client_.begin(request);
    }

    void reset() { client_.reset(); }
    void poll(RpcArbiter &arbiter) { client_.poll(arbiter); }
    bool handle_event(const RpcEvent &event) {
        return client_.handle_event(event);
    }

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

    void observe_idle(RpcArbiter &arbiter);
    void log_pressure_if_changed(RpcArbiter &arbiter);

private:
    SpoolClient client_;
    ReportSpoolPressureMonitor pressure_;
};

}  // namespace aircannect
