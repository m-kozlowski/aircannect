#pragma once

#include <stdint.h>

namespace aircannect {

class EventBroker;
class RpcArbiter;
class StreamBroker;

class RpcQuiesceCoordinator {
public:
    RpcQuiesceCoordinator(RpcArbiter &transport,
                          EventBroker &events,
                          StreamBroker &streams);

    void update(bool requested, uint32_t now_ms);

    bool complete() const;
    bool timed_out(uint32_t now_ms) const;
    bool reboot_allowed(uint32_t now_ms) const;

private:
    void begin(uint32_t now_ms);
    void end(uint32_t now_ms);
    void log_timeout();

    RpcArbiter &transport_;
    EventBroker &events_;
    StreamBroker &streams_;

    bool requested_ = false;
    bool timeout_logged_ = false;
    uint32_t deadline_ms_ = 0;
};

}  // namespace aircannect
