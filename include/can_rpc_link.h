#pragma once

#include <stdint.h>

#include "can_datagram.h"
#include "can_driver.h"
#include "fixed_queue.h"
#include "rpc_application_link.h"

namespace aircannect {

using As11ServiceFrameObserver = void (*)(void *context,
                                          const RawCanFrame &frame,
                                          uint32_t now_ms);

enum class CanSideEventKind : uint8_t {
    DebugPayload,
    DebugFramingError,
    BootNotification,
    ApplicationReset,
};

struct CanSideEvent {
    CanSideEventKind kind = CanSideEventKind::DebugPayload;
    RpcPayloadRef payload;
    std::string detail;
};

struct CanQuiesceStatus {
    bool debug_log_rx_enabled = true;
    bool debug_log_filter_pending = false;
};

class CanControlPort {
public:
    virtual ~CanControlPort() = default;

    virtual bool recover_can(const char *reason) = 0;
    virtual void request_debug_log_rx(bool enabled) = 0;
    virtual CanQuiesceStatus can_quiesce_status() const = 0;
};

class CanRpcLink final : public RpcApplicationLink,
                         public CanControlPort {
public:
    explicit CanRpcLink(CanDriver &can) : can_(can) {}

    // Physical CAN lifecycle
    bool begin() override;
    void poll(uint32_t now_ms) override;
    size_t drain_rx();

    // Application RPC link
    RpcLinkSendResult send(RpcPayloadView payload) override;
    bool take_event(RpcLinkEvent &event) override;
    void reset() override;
    RpcApplicationLinkStatus status() const override;
    const char *name() const override { return "can"; }

    // CAN side channels and maintenance
    bool take_side_event(CanSideEvent &event);
    void set_service_frame_observer(As11ServiceFrameObserver observer,
                                    void *context);
    bool recover_can(const char *reason) override;
    void request_debug_log_rx(bool enabled) override;
    CanQuiesceStatus can_quiesce_status() const override;

private:
    static bool enqueue_datagram_frame(void *context,
                                       const DatagramFrame &frame);

    void handle_frame(const RawCanFrame &frame, uint32_t now_ms);
    void handle_application_frame(const RawCanFrame &frame, uint32_t now_ms);
    void handle_debug_frame(const RawCanFrame &frame, uint32_t now_ms);
    void poll_debug_log_rx_filter();
    void push_link_error(const char *detail);
    void push_side_error(const char *detail);
    void push_boot_notification(const RawCanFrame &frame);

    CanDriver &can_;
    DatagramRx rpc_rx_{AC_STREAM_FRAME_RAW_MAX};
    DatagramRx log_rx_;
    FixedQueue<RpcLinkEvent, AC_RPC_PAYLOAD_QUEUE_DEPTH> link_events_;
    FixedQueue<CanSideEvent, AC_RPC_EVENT_QUEUE_DEPTH> side_events_;

    As11ServiceFrameObserver service_frame_observer_ = nullptr;
    void *service_frame_context_ = nullptr;
    bool debug_log_rx_requested_ = true;
};

}  // namespace aircannect
