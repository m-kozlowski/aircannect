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
        case State::EntryWaitingQuiesce: return "entry_waiting_quiesce";
        case State::EntryWaitingResetDrain:
            return "entry_waiting_reset_drain";
        case State::EntryProbing: return "entry_probing";
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

bool As11ServiceManager::acquire(As11ServiceOwner owner) {
    if (owner == As11ServiceOwner::None) return false;
    if (owner_ == owner) return true;
    if (owner_ != As11ServiceOwner::None || state_ != State::Idle) {
        return false;
    }

    owner_ = owner;
    return true;
}

void As11ServiceManager::release(As11ServiceOwner owner) {
    if (owner == As11ServiceOwner::None || owner_ != owner) return;

    cancel();
    owner_ = As11ServiceOwner::None;
}

bool As11ServiceManager::submit_packet(
    As11ServiceOwner owner,
    std::unique_ptr<LargeByteBuffer> request,
    bool enter_allowed,
    uint32_t now_ms) {
    if (owner == As11ServiceOwner::None || owner_ != owner) {
        error_ = As11ServiceTransactionError::Busy;
        return false;
    }
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

    if (header.command == AS11_SERVICE_COMMAND_ENTER) {
        if (!enter_allowed || entry_session_owned_ ||
            header.status != AS11_SERVICE_STATUS_OK ||
            header.payload_length != 0) {
            error_ = enter_allowed && !entry_session_owned_
                ? As11ServiceTransactionError::InvalidRequest
                : As11ServiceTransactionError::Busy;
            return false;
        }
        return begin_enter(header, now_ms);
    }

    if (header.status != AS11_SERVICE_STATUS_OK) {
        error_ = As11ServiceTransactionError::RequestStatus;
        return false;
    }
    return begin_request(std::move(request), header, now_ms);
}

bool As11ServiceManager::begin_enter(
    const As11ServicePacketHeader &header,
    uint32_t now_ms) {
    entry_protocol_version_ = header.protocol_version;
    entry_sequence_ = header.sequence;
    entry_started_ms_ = now_ms;
    entry_deadline_ms_ = now_ms + AC_AS11_SERVICE_ENTRY_TIMEOUT_MS;
    if (entry_deadline_ms_ == 0) entry_deadline_ms_ = 1;

    entry_session_owned_ = true;
    entry_info_pending_ = false;
    close_after_response_ = false;
    error_ = As11ServiceTransactionError::None;
    state_ = State::EntryWaitingQuiesce;

    Log::logf(CAT_CAN, LOG_INFO,
              "[SERVICE] entry requested sequence=%u\n",
              static_cast<unsigned>(entry_sequence_));
    return true;
}

bool As11ServiceManager::begin_request(
    std::unique_ptr<LargeByteBuffer> request,
    const As11ServicePacketHeader &header,
    uint32_t now_ms) {
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
        case State::EntryWaitingQuiesce:
        case State::EntryWaitingResetDrain:
        case State::EntryProbing:
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

bool As11ServiceManager::enqueue_entry_burst() {
    RawCanFrame frame;
    frame.id = AC_AS11_SERVICE_ENTRY_BURST_ID;
    frame.len = sizeof(frame.data);
    memset(frame.data, 0, sizeof(frame.data));
    return can_.enqueue_tx(frame);
}

bool As11ServiceManager::begin_entry_probe(uint32_t now_ms) {
    const size_t crc_bytes =
        as11_service_packet_crc_bytes(entry_protocol_version_);
    const size_t request_size =
        AS11_SERVICE_PACKET_HEADER_BYTES + crc_bytes;

    request_ = LargeByteBuffer::allocate(request_size);
    reassembly_buffer_ =
        LargeByteBuffer::allocate(AS11_SERVICE_PACKET_MAX_BYTES);
    if (!request_ || !reassembly_buffer_) {
        fail(As11ServiceTransactionError::AllocationFailed);
        return false;
    }

    size_t encoded_size = 0;
    if (!as11_service_encode_packet(
            entry_protocol_version_, AS11_SERVICE_COMMAND_INFO,
            AS11_SERVICE_STATUS_OK, entry_sequence_, nullptr, 0,
            request_->data(), request_->size(), encoded_size) ||
        encoded_size != request_->size()) {
        fail(As11ServiceTransactionError::InvalidRequest);
        return false;
    }

    request_command_ = AS11_SERVICE_COMMAND_INFO;
    request_sequence_ = entry_sequence_;
    request_started_ms_ = now_ms;
    phase_activity_ms_ = now_ms;
    next_entry_probe_ms_ =
        now_ms + AC_AS11_SERVICE_ENTRY_PROBE_INTERVAL_MS;
    request_frame_count_ = 0;
    response_frame_count_ = 0;
    entry_info_pending_ = true;
    receiver_.reset();
    transmitter_.reset();

    can_.set_ack_gap_expected(true);
    state_ = State::EntryProbing;
    return true;
}

bool As11ServiceManager::send_entry_info_probe(uint32_t now_ms) {
    if (!request_ || !can_.tx_idle()) return false;

    As11IsoTpCanFrame first_frame;
    transmitter_.reset();
    if (!transmitter_.begin(request_->data(), request_->size(),
                            first_frame) ||
        !enqueue_frame(first_frame)) {
        fail(As11ServiceTransactionError::CanEnqueueFailed);
        return false;
    }

    request_frame_count_++;
    phase_activity_ms_ = now_ms;
    next_entry_probe_ms_ =
        now_ms + AC_AS11_SERVICE_ENTRY_PROBE_INTERVAL_MS;
    return true;
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

    if (state_ == State::EntryProbing ||
        state_ == State::WaitingRequestFlowControl) {
        if (type != As11IsoTpFrameType::FlowControl) {
            if (state_ == State::EntryProbing) return;

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

    const bool entry_ready = state_ == State::EntryProbing;
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
    if (entry_ready) {
        release_entry_can_policy();
        request_started_ms_ = now_ms;
        Log::logf(CAT_CAN, LOG_INFO,
                  "[SERVICE] entry flow control received sequence=%u\n",
                  static_cast<unsigned>(entry_sequence_));
    }
    state_ = State::SendingRequestBlock;

    Log::logf(CAT_CAN, LOG_DEBUG,
              "[SERVICE] request flow control bs=%u st_min=%u\n",
              static_cast<unsigned>(flow_control.block_size),
              static_cast<unsigned>(flow_control.st_min));
    pump_request(now_ms);
}

void As11ServiceManager::poll_entry(RpcQuiescePort &rpc,
                                    bool quiesce_ready,
                                    bool quiesce_failed,
                                    uint32_t now_ms) {
    if (state_ == State::EntryWaitingQuiesce) {
        if (quiesce_failed) {
            fail(As11ServiceTransactionError::Busy);
            return;
        }
        if (!quiesce_ready) return;

        if (!rpc.send_quiesce_request("ResetDevice",
                                      "{\"type\":\"Fast\"}")) {
            fail(As11ServiceTransactionError::CanEnqueueFailed);
            return;
        }

        state_ = State::EntryWaitingResetDrain;
        Log::logf(CAT_CAN, LOG_INFO,
                  "[SERVICE] entry ResetDevice queued sequence=%u\n",
                  static_cast<unsigned>(entry_sequence_));
        return;
    }

    if (state_ != State::EntryWaitingResetDrain) return;
    if (!rpc.quiesce_status().idle) return;

    if (begin_entry_probe(now_ms)) {
        Log::logf(CAT_CAN, LOG_INFO,
                  "[SERVICE] entry CAN burst started sequence=%u\n",
                  static_cast<unsigned>(entry_sequence_));
    }
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

    if (entry_info_pending_ &&
        !translate_entry_info_response(header, now_ms)) {
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

    if (response_) {
        reassembly_buffer_.reset();
    } else {
        response_ = LargeByteBuffer::freeze(std::move(reassembly_buffer_));
    }
    receiver_.reset();
    state_ = State::ResponseReady;
}

bool As11ServiceManager::translate_entry_info_response(
    const As11ServicePacketHeader &header,
    uint32_t now_ms) {
    if (header.status != AS11_SERVICE_STATUS_OK || !reassembly_buffer_) {
        Log::logf(CAT_CAN, LOG_WARN,
                  "[SERVICE] entry INFO rejected sequence=%u status=%u\n",
                  static_cast<unsigned>(entry_sequence_),
                  static_cast<unsigned>(header.status));
        fail(As11ServiceTransactionError::InvalidResponse);
        return false;
    }

    const uint8_t *payload =
        reassembly_buffer_->data() + AS11_SERVICE_PACKET_HEADER_BYTES;
    if (!publish_entry_response(AS11_SERVICE_STATUS_OK, payload,
                                header.payload_length, false)) {
        fail(As11ServiceTransactionError::AllocationFailed);
        return false;
    }

    entry_info_pending_ = false;
    Log::logf(CAT_CAN, LOG_INFO,
              "[SERVICE] entry ready sequence=%u elapsed_ms=%lu\n",
              static_cast<unsigned>(entry_sequence_),
              static_cast<unsigned long>(now_ms - entry_started_ms_));
    return true;
}

bool As11ServiceManager::publish_entry_response(
    uint8_t status,
    const uint8_t *payload,
    size_t payload_size,
    bool close_after_send) {
    const size_t crc_bytes =
        as11_service_packet_crc_bytes(entry_protocol_version_);
    const size_t response_size = AS11_SERVICE_PACKET_HEADER_BYTES +
                                 payload_size + crc_bytes;
    std::unique_ptr<LargeByteBuffer> response =
        LargeByteBuffer::allocate(response_size);
    if (!response) return false;

    size_t encoded_size = 0;
    if (!as11_service_encode_packet(
            entry_protocol_version_, AS11_SERVICE_COMMAND_ENTER, status,
            entry_sequence_, payload, payload_size, response->data(),
            response->size(), encoded_size) ||
        encoded_size != response->size()) {
        return false;
    }

    response_ = LargeByteBuffer::freeze(std::move(response));
    close_after_response_ = close_after_send;
    return true;
}

void As11ServiceManager::publish_entry_timeout() {
    release_entry_can_policy();
    request_.reset();
    transmitter_.reset();
    reassembly_buffer_.reset();
    receiver_.reset();

    if (!publish_entry_response(AS11_SERVICE_STATUS_ENTRY_TIMEOUT,
                                nullptr, 0, true)) {
        fail(As11ServiceTransactionError::AllocationFailed);
        return;
    }

    entry_info_pending_ = false;
    state_ = State::ResponseReady;
    Log::logf(CAT_CAN, LOG_WARN,
              "[SERVICE] entry timed out sequence=%u\n",
              static_cast<unsigned>(entry_sequence_));
}

void As11ServiceManager::release_entry_can_policy() {
    can_.set_ack_gap_expected(false);
}

void As11ServiceManager::poll(uint32_t now_ms) {
    if (state_ == State::EntryWaitingQuiesce ||
        state_ == State::EntryWaitingResetDrain ||
        state_ == State::EntryProbing) {
        if (static_cast<int32_t>(now_ms - entry_deadline_ms_) >= 0) {
            publish_entry_timeout();
            return;
        }
    }

    if (state_ == State::EntryProbing) {
        can_.set_ack_gap_expected(true);

        if (static_cast<int32_t>(now_ms - next_entry_probe_ms_) >= 0) {
            (void)send_entry_info_probe(now_ms);
        } else if (can_.tx_idle() && !enqueue_entry_burst()) {
            fail(As11ServiceTransactionError::CanEnqueueFailed);
        }
        return;
    }
    if (state_ == State::EntryWaitingQuiesce ||
        state_ == State::EntryWaitingResetDrain) {
        return;
    }

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
    As11ServiceOwner owner,
    std::shared_ptr<const LargeByteBuffer> &response,
    bool &close_after_send) {
    response.reset();
    close_after_send = false;
    if (owner == As11ServiceOwner::None || owner_ != owner) return false;
    if (state_ != State::ResponseReady || !response_) return false;

    response = std::move(response_);
    close_after_send = close_after_response_;
    clear_transaction();
    return true;
}

bool As11ServiceManager::take_error(
    As11ServiceOwner owner,
    As11ServiceTransactionError &error) {
    error = As11ServiceTransactionError::None;
    if (owner == As11ServiceOwner::None || owner_ != owner) return false;
    if (state_ != State::Failed) return false;

    error = error_;
    clear_transaction();
    return true;
}

void As11ServiceManager::fail(As11ServiceTransactionError error) {
    release_entry_can_policy();
    request_.reset();
    transmitter_.reset();
    reassembly_buffer_.reset();
    receiver_.reset();
    response_.reset();
    error_ = error;
    state_ = State::Failed;
}

void As11ServiceManager::cancel() {
    release_entry_can_policy();
    clear_transaction();
    entry_session_owned_ = false;
    entry_protocol_version_ = 0;
    entry_sequence_ = 0;
    entry_started_ms_ = 0;
    entry_deadline_ms_ = 0;
}

void As11ServiceManager::clear_transaction() {
    request_.reset();
    transmitter_.reset();
    reassembly_buffer_.reset();
    receiver_.reset();
    response_.reset();

    request_started_ms_ = 0;
    phase_activity_ms_ = 0;
    next_entry_probe_ms_ = 0;
    next_request_frame_ms_ = 0;
    request_st_min_ms_ = 0;
    request_frame_count_ = 0;
    response_frame_count_ = 0;
    request_sequence_ = 0;
    request_command_ = 0;
    entry_info_pending_ = false;
    close_after_response_ = false;
    error_ = As11ServiceTransactionError::None;
    state_ = State::Idle;
}

}  // namespace aircannect
