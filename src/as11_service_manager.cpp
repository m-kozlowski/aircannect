#include "as11_service_manager.h"

#include <string.h>

#include "board.h"
#include "debug_log.h"
#include "runtime_clock.h"

namespace aircannect {

const char *as11_service_transaction_error_name(
    As11ServiceTransactionError error) {
    switch (error) {
        case As11ServiceTransactionError::None: return "none";
        case As11ServiceTransactionError::InvalidRequest:
            return "invalid_request";
        case As11ServiceTransactionError::RequestStatus:
            return "request_status_not_zero";
        case As11ServiceTransactionError::Busy: return "busy";
        case As11ServiceTransactionError::AllocationFailed:
            return "allocation_failed";
        case As11ServiceTransactionError::CanQueueFull:
            return "can_queue_full";
        case As11ServiceTransactionError::CanEnqueueFailed:
            return "can_enqueue_failed";
        case As11ServiceTransactionError::FlowControlFailed:
            return "flow_control_failed";
        case As11ServiceTransactionError::FlowControlOverflow:
            return "flow_control_overflow";
        case As11ServiceTransactionError::FlowControlTimeout:
            return "flow_control_timeout";
        case As11ServiceTransactionError::ReassemblyFailed:
            return "reassembly_failed";
        case As11ServiceTransactionError::InvalidResponse:
            return "invalid_response";
        case As11ServiceTransactionError::ResponseMismatch:
            return "response_mismatch";
        case As11ServiceTransactionError::ResponseTimeout:
            return "response_timeout";
    }
    return "unknown";
}

const char *As11ServiceManager::state_name(State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::WaitingRequestFlowControl:
            return "waiting_request_flow_control";
        case State::SendingRequestBlock: return "sending_request_block";
        case State::WaitingResponse: return "waiting_response";
        case State::ReceivingResponse: return "receiving_response";
        case State::ResponseReady: return "response_ready";
        case State::Failed: return "failed";
    }
    return "unknown";
}

bool As11ServiceManager::submit_request(
    std::unique_ptr<LargeByteBuffer> request,
    uint32_t now_ms) {
    if (state_ != State::Idle) {
        error_ = As11ServiceTransactionError::Busy;
        return false;
    }
    if (!request || request->size() > AS11_SERVICE_PACKET_MAX_BYTES) {
        error_ = As11ServiceTransactionError::InvalidRequest;
        return false;
    }

    As11ServicePacketHeader header;
    const As11ServicePacketError packet_error =
        as11_service_validate_packet(request->data(), request->size(),
                                     header);
    if (packet_error != As11ServicePacketError::None) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] request rejected error=%s\n",
                  as11_service_packet_error_name(packet_error));
        error_ = As11ServiceTransactionError::InvalidRequest;
        return false;
    }
    if (header.status != 0) {
        error_ = As11ServiceTransactionError::RequestStatus;
        return false;
    }

    const size_t request_size = request->size();
    const size_t request_frames = as11_isotp_frame_count(request_size);

    reassembly_buffer_ =
        LargeByteBuffer::allocate(AS11_SERVICE_PACKET_MAX_BYTES);
    if (!reassembly_buffer_) {
        error_ = As11ServiceTransactionError::AllocationFailed;
        return false;
    }

    request_ = std::move(request);
    As11IsoTpCanFrame first_frame;

    if (!transmitter_.begin(request_->data(), request_->size(),
                            first_frame)) {
        clear_transaction();
        error_ = As11ServiceTransactionError::InvalidRequest;
        return false;
    }
    if (can_.tx_queue_free() == 0) {
        clear_transaction();
        error_ = As11ServiceTransactionError::CanQueueFull;
        return false;
    }
    if (!enqueue_frame(first_frame)) {
        clear_transaction();
        error_ = As11ServiceTransactionError::CanEnqueueFailed;
        return false;
    }

    request_command_ = header.command;
    request_sequence_ = header.sequence;
    request_started_ms_ = now_ms;
    phase_activity_ms_ = now_ms;
    request_frame_count_ = 1;
    response_frame_count_ = 0;
    error_ = As11ServiceTransactionError::None;
    receiver_.reset();

    if (transmitter_.complete()) {
        finish_request(now_ms);
    } else {
        state_ = State::WaitingRequestFlowControl;
    }

    Log::logf(CAT_CAN, LOG_DEBUG,
              "[SERVICE] ISO-TP request started command=%u sequence=%u "
              "bytes=%u frames=%u\n",
              static_cast<unsigned>(request_command_),
              static_cast<unsigned>(request_sequence_),
              static_cast<unsigned>(request_size),
              static_cast<unsigned>(request_frames));
    return true;
}

bool As11ServiceManager::pending() const {
    switch (state_) {
        case State::WaitingRequestFlowControl:
        case State::SendingRequestBlock:
        case State::WaitingResponse:
        case State::ReceivingResponse:
            return true;
        case State::Idle:
        case State::ResponseReady:
        case State::Failed:
            return false;
    }
    return false;
}

bool As11ServiceManager::enqueue_frame(
    const As11IsoTpCanFrame &source) {
    RawCanFrame frame;
    frame.id = AC_AS11_SERVICE_TX_ID;
    frame.len = source.len;
    memcpy(frame.data, source.data, source.len);
    return can_.enqueue_tx(frame);
}

void As11ServiceManager::accept_can_frame(const RawCanFrame &frame,
                                          uint32_t now_ms) {
    if (frame.id != AC_AS11_SERVICE_RX_ID || frame.extended || frame.remote ||
        !pending() || !reassembly_buffer_) {
        return;
    }

    As11IsoTpFrameType type;
    if (!as11_isotp_frame_type(frame.data, frame.len, type)) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] invalid ISO-TP frame phase=%s len=%u\n",
                  state_name(state_),
                  static_cast<unsigned>(frame.len));
        fail(As11ServiceTransactionError::ReassemblyFailed);
        return;
    }

    if (state_ == State::WaitingRequestFlowControl) {
        if (type != As11IsoTpFrameType::FlowControl) {
            Log::logf(CAT_CAN, LOG_WARN,
                      "[SERVICE] expected flow control got_pci=%u\n",
                      static_cast<unsigned>(type));
            fail(As11ServiceTransactionError::FlowControlFailed);
            return;
        }

        handle_request_flow_control(frame, now_ms);
        return;
    }

    if (state_ == State::WaitingResponse) {
        if (type != As11IsoTpFrameType::Single &&
            type != As11IsoTpFrameType::First) {
            Log::logf(CAT_CAN, LOG_WARN,
                      "[SERVICE] expected response start got_pci=%u\n",
                      static_cast<unsigned>(type));
            fail(As11ServiceTransactionError::ReassemblyFailed);
            return;
        }

        handle_response_frame(frame, now_ms);
        return;
    }

    if (state_ == State::ReceivingResponse) {
        if (type != As11IsoTpFrameType::Consecutive) {
            Log::logf(CAT_CAN, LOG_WARN,
                      "[SERVICE] expected consecutive frame got_pci=%u\n",
                      static_cast<unsigned>(type));
            fail(As11ServiceTransactionError::ReassemblyFailed);
            return;
        }

        handle_response_frame(frame, now_ms);
        return;
    }

    Log::logf(CAT_CAN, LOG_WARN,
              "[SERVICE] unexpected service frame phase=%s pci=%u\n",
              state_name(state_),
              static_cast<unsigned>(type));
    fail(As11ServiceTransactionError::ReassemblyFailed);
}

void As11ServiceManager::handle_request_flow_control(
    const RawCanFrame &frame,
    uint32_t now_ms) {
    As11IsoTpFlowControl flow_control;
    if (!as11_isotp_parse_flow_control(frame.data, frame.len,
                                       flow_control)) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] invalid request flow control\n");
        fail(As11ServiceTransactionError::FlowControlFailed);
        return;
    }

    phase_activity_ms_ = now_ms;
    if (flow_control.status == As11IsoTpFlowStatus::Wait) {
        Log::logf(CAT_CAN, LOG_DEBUG,
                  "[SERVICE] request flow control wait\n");
        return;
    }
    if (flow_control.status == As11IsoTpFlowStatus::Overflow) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] request flow control overflow\n");
        fail(As11ServiceTransactionError::FlowControlOverflow);
        return;
    }

    uint32_t st_min_ms = 0;
    if (!as11_isotp_st_min_delay_ms(flow_control.st_min, st_min_ms) ||
        !transmitter_.grant(flow_control)) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] request flow control rejected bs=%u st_min=%u\n",
                  static_cast<unsigned>(flow_control.block_size),
                  static_cast<unsigned>(flow_control.st_min));
        fail(As11ServiceTransactionError::FlowControlFailed);
        return;
    }

    request_st_min_ms_ = st_min_ms;
    next_request_frame_ms_ = now_ms;
    state_ = State::SendingRequestBlock;

    Log::logf(CAT_CAN, LOG_DEBUG,
              "[SERVICE] request flow control bs=%u st_min=%u\n",
              static_cast<unsigned>(flow_control.block_size),
              static_cast<unsigned>(flow_control.st_min));
    pump_request(now_ms);
}

void As11ServiceManager::pump_request(uint32_t now_ms) {
    while (state_ == State::SendingRequestBlock) {
        if (request_st_min_ms_ != 0 &&
            static_cast<int32_t>(now_ms - next_request_frame_ms_) < 0) {
            return;
        }
        if (can_.tx_queue_free() == 0) return;

        As11IsoTpCanFrame frame;
        const As11IsoTpTransmitStatus status = transmitter_.next(frame);
        if (status == As11IsoTpTransmitStatus::WaitFlowControl) {
            state_ = State::WaitingRequestFlowControl;
            phase_activity_ms_ = now_ms;
            return;
        }
        if (status == As11IsoTpTransmitStatus::Complete) {
            finish_request(now_ms);
            return;
        }
        if (status != As11IsoTpTransmitStatus::FrameReady) {
            fail(As11ServiceTransactionError::CanEnqueueFailed);
            return;
        }
        if (!enqueue_frame(frame)) {
            fail(As11ServiceTransactionError::CanEnqueueFailed);
            return;
        }

        request_frame_count_++;
        phase_activity_ms_ = now_ms;
        if (transmitter_.complete()) {
            finish_request(now_ms);
            return;
        }
        if (transmitter_.waiting_flow_control()) {
            state_ = State::WaitingRequestFlowControl;
            return;
        }
        if (request_st_min_ms_ != 0) {
            next_request_frame_ms_ = now_ms + request_st_min_ms_;
            return;
        }
    }
}

void As11ServiceManager::finish_request(uint32_t now_ms) {
    const uint16_t frame_count = request_frame_count_;
    transmitter_.reset();
    request_.reset();

    request_st_min_ms_ = 0;
    next_request_frame_ms_ = 0;
    phase_activity_ms_ = now_ms;
    state_ = State::WaitingResponse;

    Log::logf(CAT_CAN, LOG_DEBUG,
              "[SERVICE] ISO-TP request complete command=%u sequence=%u "
              "frames=%u elapsed_ms=%lu\n",
              static_cast<unsigned>(request_command_),
              static_cast<unsigned>(request_sequence_),
              static_cast<unsigned>(frame_count),
              static_cast<unsigned long>(now_ms - request_started_ms_));
}

void As11ServiceManager::handle_response_frame(const RawCanFrame &frame,
                                               uint32_t now_ms) {
    response_frame_count_++;
    if (response_frame_count_ == 1) {
        Log::logf(CAT_CAN, LOG_DEBUG,
                  "[SERVICE] ISO-TP response started command=%u sequence=%u "
                  "elapsed_ms=%lu\n",
                  static_cast<unsigned>(request_command_),
                  static_cast<unsigned>(request_sequence_),
                  static_cast<unsigned long>(now_ms - request_started_ms_));
    }

    const As11IsoTpReceiveResult result = receiver_.feed(
        frame.data, frame.len, reassembly_buffer_->data(),
        reassembly_buffer_->size(), AS11_SERVICE_RESPONSE_BLOCK_SIZE,
        now_ms);
    phase_activity_ms_ = now_ms;

    if (result.status == As11IsoTpReceiveStatus::Error) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] response ISO-TP failed command=%u sequence=%u "
                  "frames=%u error=%s\n",
                  static_cast<unsigned>(request_command_),
                  static_cast<unsigned>(request_sequence_),
                  static_cast<unsigned>(response_frame_count_),
                  as11_isotp_receive_error_name(result.error));
        fail(As11ServiceTransactionError::ReassemblyFailed);
        return;
    }

    if (result.status == As11IsoTpReceiveStatus::FlowControlNeeded) {
        if (!send_response_flow_control()) return;
        state_ = State::ReceivingResponse;
        return;
    }

    if (result.status == As11IsoTpReceiveStatus::Complete) {
        handle_complete_response(result.packet_size, now_ms);
    }
}

bool As11ServiceManager::send_response_flow_control() {
    if (can_.tx_queue_free() == 0) {
        fail(As11ServiceTransactionError::CanQueueFull);
        return false;
    }

    As11IsoTpCanFrame frame;
    if (!as11_isotp_make_flow_control(AS11_SERVICE_RESPONSE_BLOCK_SIZE,
                                      AS11_SERVICE_RESPONSE_ST_MIN,
                                      frame) ||
        !enqueue_frame(frame)) {
        fail(As11ServiceTransactionError::CanEnqueueFailed);
        return false;
    }

    Log::logf(CAT_CAN, LOG_DEBUG,
              "[SERVICE] response flow control bs=%u st_min=%u\n",
              static_cast<unsigned>(AS11_SERVICE_RESPONSE_BLOCK_SIZE),
              static_cast<unsigned>(AS11_SERVICE_RESPONSE_ST_MIN));
    return true;
}

void As11ServiceManager::handle_complete_response(size_t packet_size,
                                                  uint32_t now_ms) {
    if (!reassembly_buffer_ ||
        !reassembly_buffer_->truncate(packet_size)) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] response rejected command=%u sequence=%u "
                  "frames=%u error=invalid_size\n",
                  static_cast<unsigned>(request_command_),
                  static_cast<unsigned>(request_sequence_),
                  static_cast<unsigned>(response_frame_count_));
        fail(As11ServiceTransactionError::InvalidResponse);
        return;
    }

    As11ServicePacketHeader header;
    const As11ServicePacketError packet_error =
        as11_service_validate_packet(reassembly_buffer_->data(),
                                     reassembly_buffer_->size(), header);
    if (packet_error != As11ServicePacketError::None) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] response rejected command=%u sequence=%u "
                  "bytes=%u frames=%u error=%s\n",
                  static_cast<unsigned>(request_command_),
                  static_cast<unsigned>(request_sequence_),
                  static_cast<unsigned>(packet_size),
                  static_cast<unsigned>(response_frame_count_),
                  as11_service_packet_error_name(packet_error));
        fail(As11ServiceTransactionError::InvalidResponse);
        return;
    }
    if (header.command != request_command_ ||
        header.sequence != request_sequence_) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] response mismatch command=%u/%u "
                  "sequence=%u/%u frames=%u elapsed_ms=%lu\n",
                  static_cast<unsigned>(header.command),
                  static_cast<unsigned>(request_command_),
                  static_cast<unsigned>(header.sequence),
                  static_cast<unsigned>(request_sequence_),
                  static_cast<unsigned>(response_frame_count_),
                  static_cast<unsigned long>(now_ms - request_started_ms_));
        fail(As11ServiceTransactionError::ResponseMismatch);
        return;
    }

    Log::logf(CAT_CAN, LOG_DEBUG,
              "[SERVICE] response complete command=%u sequence=%u status=%u "
              "bytes=%u frames=%u elapsed_ms=%lu\n",
              static_cast<unsigned>(header.command),
              static_cast<unsigned>(header.sequence),
              static_cast<unsigned>(header.status),
              static_cast<unsigned>(packet_size),
              static_cast<unsigned>(response_frame_count_),
              static_cast<unsigned long>(now_ms - request_started_ms_));

    response_ = LargeByteBuffer::freeze(std::move(reassembly_buffer_));
    receiver_.reset();
    state_ = State::ResponseReady;
}

void As11ServiceManager::poll(uint32_t now_ms) {
    if (state_ == State::SendingRequestBlock) pump_request(now_ms);
    if (!pending()) return;

    if (millis_elapsed_at_least(now_ms, request_started_ms_,
                                AC_AS11_SERVICE_RESPONSE_TIMEOUT_MS)) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] transaction timeout command=%u sequence=%u "
                  "phase=%s tx_frames=%u rx_frames=%u elapsed_ms=%lu\n",
                  static_cast<unsigned>(request_command_),
                  static_cast<unsigned>(request_sequence_),
                  state_name(state_),
                  static_cast<unsigned>(request_frame_count_),
                  static_cast<unsigned>(response_frame_count_),
                  static_cast<unsigned long>(now_ms - request_started_ms_));
        fail(As11ServiceTransactionError::ResponseTimeout);
        return;
    }

    if (state_ == State::WaitingRequestFlowControl &&
        millis_elapsed_at_least(now_ms, phase_activity_ms_,
                                AC_AS11_SERVICE_REASSEMBLY_TIMEOUT_MS)) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] flow control timeout command=%u sequence=%u "
                  "tx_frames=%u elapsed_ms=%lu\n",
                  static_cast<unsigned>(request_command_),
                  static_cast<unsigned>(request_sequence_),
                  static_cast<unsigned>(request_frame_count_),
                  static_cast<unsigned long>(now_ms - request_started_ms_));
        fail(As11ServiceTransactionError::FlowControlTimeout);
        return;
    }

    if (state_ != State::ReceivingResponse) return;

    const As11IsoTpReceiveResult timeout = receiver_.poll(
        now_ms, AC_AS11_SERVICE_REASSEMBLY_TIMEOUT_MS);
    if (timeout.status != As11IsoTpReceiveStatus::Error) return;

    Log::logf(CAT_CAN, LOG_WARN,
              "[SERVICE] response ISO-TP timeout command=%u sequence=%u "
              "frames=%u elapsed_ms=%lu\n",
              static_cast<unsigned>(request_command_),
              static_cast<unsigned>(request_sequence_),
              static_cast<unsigned>(response_frame_count_),
              static_cast<unsigned long>(now_ms - request_started_ms_));
    fail(As11ServiceTransactionError::ReassemblyFailed);
}

bool As11ServiceManager::take_response(
    std::shared_ptr<const LargeByteBuffer> &response) {
    response.reset();
    if (state_ != State::ResponseReady || !response_) return false;

    response = std::move(response_);
    clear_transaction();
    return true;
}

bool As11ServiceManager::take_error(
    As11ServiceTransactionError &error) {
    error = As11ServiceTransactionError::None;
    if (state_ != State::Failed) return false;

    error = error_;
    clear_transaction();
    return true;
}

void As11ServiceManager::fail(As11ServiceTransactionError error) {
    request_.reset();
    transmitter_.reset();
    reassembly_buffer_.reset();
    receiver_.reset();
    response_.reset();
    error_ = error;
    state_ = State::Failed;
}

void As11ServiceManager::cancel() {
    clear_transaction();
}

void As11ServiceManager::clear_transaction() {
    request_.reset();
    transmitter_.reset();
    reassembly_buffer_.reset();
    receiver_.reset();
    response_.reset();

    request_started_ms_ = 0;
    phase_activity_ms_ = 0;
    next_request_frame_ms_ = 0;
    request_st_min_ms_ = 0;
    request_frame_count_ = 0;
    response_frame_count_ = 0;
    request_sequence_ = 0;
    request_command_ = 0;
    error_ = As11ServiceTransactionError::None;
    state_ = State::Idle;
}

}  // namespace aircannect
