#pragma once

#include <stdint.h>

namespace aircannect {

class SpoolClient;

class ReportSpoolPressureMonitor {
public:
    void observe_idle(uint32_t rx_queue_full_alerts);
    void log_if_changed(uint32_t rx_queue_full_alerts,
                        const SpoolClient &spool);

private:
    uint32_t observed_rx_queue_full_alerts_ = 0;
};

}  // namespace aircannect
