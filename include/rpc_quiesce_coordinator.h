#pragma once

#include <stdint.h>

#include "can_control_port.h"
#include "event_broker.h"
#include "rpc_transport_ports.h"

namespace aircannect {

class StreamBroker;

class RpcQuiesceCoordinator {
public:
    RpcQuiesceCoordinator(RpcQuiescePort &transport,
                          CanControlPort &can,
                          EventBroker &events,
                          StreamBroker &streams);

    bool begin();
    void update(bool requested,
                bool controlled_disconnect_required,
                bool ble_selected,
                uint32_t now_ms);

    // Intentional connection hold, shared by local commands and AS11.
    void request_connection_hold() { connection_held_ = true; }
    void release_connection_hold() { connection_held_ = false; }
    bool connection_held() const { return connection_held_; }

    bool complete() const;
    bool timed_out() const;
    bool shutdown_allowed() const;
    bool reboot_allowed() const;
    bool requested() const { return requested_; }

private:
    static void event_observer(void *context, const As11EventFrame &frame,
                               uint32_t now_ms);
    void configure_ble_control(bool selected);

    void begin(uint32_t now_ms);
    void end(uint32_t now_ms);
    void poll_quiesce(uint32_t now_ms);
    void poll_controlled_disconnect(bool required,
                                    uint32_t now_ms);
    void log_timeout();
    bool push_traffic_quiesced(const RpcQuiesceStatus &transport) const;

    RpcQuiescePort &transport_;
    CanControlPort &can_;
    EventBroker &events_;
    StreamBroker &streams_;

    bool event_observer_registered_ = false;
    bool ble_selected_ = false;
    bool connection_held_ = false;
    EventConsumerHandle ble_events_ = EVENT_CONSUMER_INVALID;

    bool requested_ = false;
    bool complete_ = false;
    bool timed_out_ = false;
    uint32_t deadline_ms_ = 0;

    bool controlled_disconnect_required_ = false;
    bool disconnect_requested_ = false;
    bool disconnect_complete_ = false;
    bool disconnect_timed_out_ = false;
    uint32_t disconnect_deadline_ms_ = 0;
};

}  // namespace aircannect
