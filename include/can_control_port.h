#pragma once

namespace aircannect {

struct CanQuiesceStatus {
    bool debug_log_rx_enabled = true;
    bool debug_log_filter_pending = false;
};

class CanControlPort {
public:
    virtual ~CanControlPort() = default;

    virtual bool can_available() const = 0;
    virtual bool recover_can(const char *reason) = 0;
    virtual void request_debug_log_rx(bool enabled) = 0;
    virtual CanQuiesceStatus can_quiesce_status() const = 0;
};

}  // namespace aircannect
