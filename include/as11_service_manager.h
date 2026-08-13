#pragma once

#include <memory>
#include <stdint.h>

#include "as11_service_protocol.h"
#include "can_driver.h"
#include "large_byte_buffer.h"
#include "rpc_transport_ports.h"

namespace aircannect {

enum class As11ServiceTransactionError : uint8_t {
    None,
    InvalidRequest,
    RequestStatus,
    Busy,
    AllocationFailed,
    CanQueueFull,
    CanEnqueueFailed,
    FlowControlFailed,
    FlowControlOverflow,
    FlowControlTimeout,
    ReassemblyFailed,
    InvalidResponse,
    ResponseMismatch,
    ResponseTimeout,
};

const char *as11_service_transaction_error_name(
    As11ServiceTransactionError error);

enum class As11ServiceOwner : uint8_t {
    None,
    TcpBridge,
    ResmedOta,
};

class As11ServiceManager {
public:
    explicit As11ServiceManager(CanDriver &can) : can_(can) {}

    // Session ownership
    bool acquire(As11ServiceOwner owner);
    void release(As11ServiceOwner owner);
    bool owned_by(As11ServiceOwner owner) const { return owner_ == owner; }

    // Transaction lifecycle
    bool submit_packet(As11ServiceOwner owner,
                       std::unique_ptr<LargeByteBuffer> request,
                       bool enter_allowed,
                       uint32_t now_ms);
    void poll(uint32_t now_ms);
    void poll_entry(RpcQuiescePort &rpc,
                    bool quiesce_ready,
                    bool quiesce_failed,
                    uint32_t now_ms);

    // CAN ingress
    void accept_can_frame(const RawCanFrame &frame, uint32_t now_ms);

    // Completion
    bool take_response(As11ServiceOwner owner,
                       std::shared_ptr<const LargeByteBuffer> &response,
                       bool &close_after_send);
    bool take_error(As11ServiceOwner owner,
                    As11ServiceTransactionError &error);
    bool pending() const;
    bool exclusive_requested() const { return entry_session_owned_; }
    As11ServiceTransactionError last_error() const { return error_; }

private:
    enum class State : uint8_t {
        Idle,
        EntryWaitingQuiesce,
        EntryWaitingResetDrain,
        EntryProbing,
        WaitingRequestFlowControl,
        SendingRequestBlock,
        WaitingResponse,
        ReceivingResponse,
        ResponseReady,
        Failed,
    };

    // Request transmission
    bool begin_enter(const As11ServicePacketHeader &header,
                     uint32_t now_ms);
    bool begin_request(std::unique_ptr<LargeByteBuffer> request,
                       const As11ServicePacketHeader &header,
                       uint32_t now_ms);
    bool enqueue_frame(const As11IsoTpCanFrame &frame);
    bool enqueue_entry_burst();
    bool begin_entry_probe(uint32_t now_ms);
    bool send_entry_info_probe(uint32_t now_ms);
    void handle_request_flow_control(const RawCanFrame &frame,
                                     uint32_t now_ms);
    void pump_request(uint32_t now_ms);
    void finish_request(uint32_t now_ms);

    // Response reception
    void handle_response_frame(const RawCanFrame &frame,
                               uint32_t now_ms);
    bool send_response_flow_control();
    void handle_complete_response(size_t packet_size, uint32_t now_ms);
    bool translate_entry_info_response(
        const As11ServicePacketHeader &header,
        uint32_t now_ms);

    // Completion and failure
    bool publish_entry_response(uint8_t status,
                                const uint8_t *payload,
                                size_t payload_size,
                                bool close_after_send);
    void publish_entry_timeout();
    void release_entry_can_policy();
    static const char *state_name(State state);
    void fail(As11ServiceTransactionError error);
    void cancel();
    void clear_transaction();

    CanDriver &can_;

    std::unique_ptr<LargeByteBuffer> request_;
    As11IsoTpTransmitter transmitter_;

    std::unique_ptr<LargeByteBuffer> reassembly_buffer_;
    As11IsoTpReceiver receiver_;
    std::shared_ptr<const LargeByteBuffer> response_;

    uint32_t request_started_ms_ = 0;
    uint32_t phase_activity_ms_ = 0;
    uint32_t entry_started_ms_ = 0;
    uint32_t entry_deadline_ms_ = 0;
    uint32_t next_entry_probe_ms_ = 0;
    uint32_t next_request_frame_ms_ = 0;
    uint32_t request_st_min_ms_ = 0;
    uint16_t request_frame_count_ = 0;
    uint16_t response_frame_count_ = 0;
    uint16_t request_sequence_ = 0;
    uint16_t entry_sequence_ = 0;
    uint8_t request_command_ = 0;
    uint8_t entry_protocol_version_ = 0;
    As11ServiceTransactionError error_ =
        As11ServiceTransactionError::None;
    bool entry_session_owned_ = false;
    bool entry_info_pending_ = false;
    bool close_after_response_ = false;
    As11ServiceOwner owner_ = As11ServiceOwner::None;
    State state_ = State::Idle;
};

}  // namespace aircannect
