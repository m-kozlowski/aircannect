#pragma once

#include <stdint.h>

#include "can_datagram.h"
#include "can_control_port.h"
#include "can_driver.h"
#include "fixed_queue.h"
#include "airmini_ncp_clock.h"
#include "airmini_ncp_clock_port.h"
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

class CanRpcLink final : public RpcApplicationLink,
                         public CanControlPort,
                         public AirMiniNcpClockPort {
    enum class NcpClockPhase : uint8_t {
        Idle,
        WaitingSet,
        WaitingWriteReadback,
    };

public:
    explicit CanRpcLink(CanDriver &can) : can_(can) {}

    // Physical CAN lifecycle
    bool begin() override;
    void poll(uint32_t now_ms) override;
    bool set_physical_enabled(bool enabled);
    bool physical_enabled() const { return physical_enabled_; }
    void poll_physical(uint32_t now_ms);
    size_t drain_rx();

    // Application RPC link
    RpcLinkSendResult send(RpcPayloadView payload) override;
    bool take_event(RpcLinkEvent &event) override;
    void reset() override;
    void set_peer_absence_expected(bool expected) override;
    RpcApplicationLinkStatus status() const override;
    const char *name() const override { return "can"; }
    void set_application_enabled(bool enabled);

    // CAN side channels and maintenance
    bool take_side_event(CanSideEvent &event);
    void set_service_frame_observer(As11ServiceFrameObserver observer,
                                    void *context);
    bool can_available() const override { return physical_enabled(); }
    bool recover_can(const char *reason) override;
    void request_debug_log_rx(bool enabled) override;
    CanQuiesceStatus can_quiesce_status() const override;

    // AirMini NCP clock lane. It shares the CAN driver's TX/RX pump.
    bool available() const override {
        return physical_enabled() && application_enabled_;
    }
    bool request_write(const char *datetime, uint32_t now_ms) override;
    bool take_result(AirMiniNcpClockResult &result) override;
    bool pending() const override;
    void cancel(const char *reason) override;

private:
    static bool enqueue_datagram_frame(void *context,
                                       const DatagramFrame &frame);
    static bool enqueue_ncp_frame(void *context,
                                  const DatagramFrame &frame);

    void handle_frame(const RawCanFrame &frame, uint32_t now_ms);
    void handle_application_frame(const RawCanFrame &frame, uint32_t now_ms);
    void handle_debug_frame(const RawCanFrame &frame, uint32_t now_ms);
    void poll_debug_log_rx_filter();
    void push_link_error(const char *detail);
    void push_side_error(const char *detail);
    void push_boot_notification(const RawCanFrame &frame);
    bool send_ncp_record(AirMiniNcpClockCommand command,
                         const char *datetime,
                         uint32_t now_ms);
    void poll_ncp_clock(uint32_t now_ms);
    void handle_ncp_clock_frame(const RawCanFrame &frame,
                                uint32_t now_ms);
    void finish_ncp_clock(bool succeeded,
                          const char *reason,
                          int16_t error_code = 0,
                          const char *datetime = nullptr);

    CanDriver &can_;
    DatagramRx rpc_rx_{AC_STREAM_FRAME_RAW_MAX};
    DatagramRx log_rx_;
    DatagramRx ncp_clock_rx_{64};
    FixedQueue<RpcLinkEvent, AC_CAN_ENABLED ? AC_RPC_PAYLOAD_QUEUE_DEPTH : 1>
        link_events_;
    FixedQueue<CanSideEvent, AC_CAN_ENABLED ? AC_RPC_EVENT_QUEUE_DEPTH : 1>
        side_events_;

    As11ServiceFrameObserver service_frame_observer_ = nullptr;
    void *service_frame_context_ = nullptr;
    bool physical_enabled_ = false;
    bool application_enabled_ = true;
    bool debug_log_rx_requested_ = true;

    NcpClockPhase ncp_clock_phase_ = NcpClockPhase::Idle;
    uint8_t ncp_clock_tag_ = 0;
    uint8_t ncp_clock_expected_tag_ = 0;
    uint32_t ncp_clock_deadline_ms_ = 0;
    uint32_t ncp_clock_write_started_ms_ = 0;
    std::string ncp_clock_requested_datetime_;
    AirMiniNcpClockResult ncp_clock_result_;
    bool ncp_clock_result_pending_ = false;
};

}  // namespace aircannect
