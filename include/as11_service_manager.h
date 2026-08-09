#pragma once

#include <memory>
#include <stdint.h>

#include "as11_service_protocol.h"
#include "can_driver.h"
#include "large_byte_buffer.h"

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

class As11ServiceManager {
public:
    explicit As11ServiceManager(CanDriver &can) : can_(can) {}

    // Transaction lifecycle
    bool submit_request(std::unique_ptr<LargeByteBuffer> request,
                        uint32_t now_ms);
    void cancel();
    void poll(uint32_t now_ms);

    // CAN ingress
    void accept_can_frame(const RawCanFrame &frame, uint32_t now_ms);

    // Completion
    bool take_response(std::shared_ptr<const LargeByteBuffer> &response);
    bool take_error(As11ServiceTransactionError &error);
    bool pending() const;
    As11ServiceTransactionError last_error() const { return error_; }

private:
    enum class State : uint8_t {
        Idle,
        WaitingRequestFlowControl,
        SendingRequestBlock,
        WaitingResponse,
        ReceivingResponse,
        ResponseReady,
        Failed,
    };

    // Request transmission
    bool enqueue_frame(const As11IsoTpCanFrame &frame);
    void handle_request_flow_control(const RawCanFrame &frame,
                                     uint32_t now_ms);
    void pump_request(uint32_t now_ms);
    void finish_request(uint32_t now_ms);

    // Response reception
    void handle_response_frame(const RawCanFrame &frame,
                               uint32_t now_ms);
    bool send_response_flow_control();
    void handle_complete_response(size_t packet_size, uint32_t now_ms);

    // Completion and failure
    static const char *state_name(State state);
    void fail(As11ServiceTransactionError error);
    void clear_transaction();

    CanDriver &can_;

    std::unique_ptr<LargeByteBuffer> request_;
    As11IsoTpTransmitter transmitter_;

    std::unique_ptr<LargeByteBuffer> reassembly_buffer_;
    As11IsoTpReceiver receiver_;
    std::shared_ptr<const LargeByteBuffer> response_;

    uint32_t request_started_ms_ = 0;
    uint32_t phase_activity_ms_ = 0;
    uint32_t next_request_frame_ms_ = 0;
    uint32_t request_st_min_ms_ = 0;
    uint16_t request_frame_count_ = 0;
    uint16_t response_frame_count_ = 0;
    uint16_t request_sequence_ = 0;
    uint8_t request_command_ = 0;
    As11ServiceTransactionError error_ =
        As11ServiceTransactionError::None;
    State state_ = State::Idle;
};

}  // namespace aircannect
