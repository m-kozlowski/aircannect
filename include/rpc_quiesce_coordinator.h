#pragma once

#include <stdint.h>

#include "rpc_transport_ports.h"

namespace aircannect {

class EventBroker;
class StreamBroker;

class RpcQuiesceCoordinator {
public:
    RpcQuiesceCoordinator(RpcQuiescePort &transport,
                          EventBroker &events,
                          StreamBroker &streams);

    void update(bool requested, uint32_t now_ms);

    bool complete() const;
    bool timed_out() const;
    bool reboot_allowed() const;

private:
    void begin(uint32_t now_ms);
    void end(uint32_t now_ms);
    void log_timeout();
    bool push_traffic_quiesced(const RpcQuiesceStatus &transport) const;

    RpcQuiescePort &transport_;
    EventBroker &events_;
    StreamBroker &streams_;

    bool requested_ = false;
    bool complete_ = false;
    bool timed_out_ = false;
    uint32_t deadline_ms_ = 0;
};

}  // namespace aircannect
