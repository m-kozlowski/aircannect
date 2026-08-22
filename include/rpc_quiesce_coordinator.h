#pragma once

#include <stdint.h>

#include "can_rpc_link.h"
#include "rpc_transport_ports.h"

namespace aircannect {

class EventBroker;
class StreamBroker;

class RpcQuiesceCoordinator {
public:
    RpcQuiesceCoordinator(RpcQuiescePort &transport,
                          CanControlPort &can,
                          EventBroker &events,
                          StreamBroker &streams);

    void update(bool requested,
                bool restart_requested,
                uint32_t now_ms);

    bool complete() const;
    bool timed_out() const;
    bool reboot_allowed() const;
    bool requested() const { return requested_; }

private:
    void begin(uint32_t now_ms);
    void end(uint32_t now_ms);
    void poll_quiesce(uint32_t now_ms);
    void poll_controlled_disconnect(bool restart_requested,
                                    uint32_t now_ms);
    void log_timeout();
    bool push_traffic_quiesced(const RpcQuiesceStatus &transport) const;

    RpcQuiescePort &transport_;
    CanControlPort &can_;
    EventBroker &events_;
    StreamBroker &streams_;

    bool requested_ = false;
    bool complete_ = false;
    bool timed_out_ = false;
    uint32_t deadline_ms_ = 0;

    bool restart_requested_ = false;
    bool disconnect_requested_ = false;
    bool disconnect_complete_ = false;
    bool disconnect_timed_out_ = false;
    uint32_t disconnect_deadline_ms_ = 0;
};

}  // namespace aircannect
