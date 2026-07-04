#pragma once

#include <stdint.h>

namespace aircannect {

class RpcArbiter;
class SpoolClient;

class ReportSpoolPressureMonitor {
public:
    void observe_idle(const RpcArbiter &arbiter);
    void log_if_changed(const RpcArbiter &arbiter, const SpoolClient &spool);

private:
    uint32_t observed_rx_queue_full_alerts_ = 0;
};

}  // namespace aircannect
