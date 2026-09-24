#pragma once

namespace aircannect {

enum class CanRxMode {
    All,
    Application,
    AckOnly,
};

struct CanQuiesceStatus {
    CanRxMode rx_mode = CanRxMode::All;
    bool filter_pending = false;
};

class CanControlPort {
public:
    virtual ~CanControlPort() = default;

    virtual bool can_available() const = 0;
    virtual bool recover_can(const char *reason) = 0;
    virtual void request_rx_mode(CanRxMode mode) = 0;
    virtual CanQuiesceStatus can_quiesce_status() const = 0;
};

}  // namespace aircannect
