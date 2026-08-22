#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "rpc_payload.h"

namespace aircannect {

enum class RpcLinkSendResult : uint8_t {
    Accepted,
    Busy,
    Unavailable,
    Failed,
};

enum class RpcLinkEventKind : uint8_t {
    Payload,
    FramingError,
    Disconnected,
    TransportChanged,
};

struct RpcLinkEvent {
    RpcLinkEventKind kind = RpcLinkEventKind::Payload;
    RpcPayloadRef payload;
    std::string detail;
};

struct RpcApplicationLinkStatus {
    bool ready = false;
    bool tx_idle = true;
    bool rx_pressure = false;
    size_t tx_queue_depth = 0;
    uint32_t rx_pressure_events = 0;
};

class RpcApplicationLink {
public:
    virtual ~RpcApplicationLink() = default;

    virtual bool begin() = 0;
    virtual void poll(uint32_t now_ms) = 0;
    virtual RpcLinkSendResult send(RpcPayloadView payload) = 0;
    virtual bool take_event(RpcLinkEvent &event) = 0;
    virtual void reset() = 0;

    virtual RpcApplicationLinkStatus status() const = 0;
    virtual const char *name() const = 0;
};

}  // namespace aircannect
