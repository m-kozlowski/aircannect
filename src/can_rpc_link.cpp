#include "can_rpc_link.h"

#include <stdio.h>
#include <utility>

#include "board.h"
#include "debug_log.h"
#include "hex_util.h"
#include "utc_time.h"

namespace aircannect {
namespace {

constexpr uint32_t AIRMINI_NCP_TX_ID = 0x381;
constexpr uint32_t AIRMINI_NCP_RX_ID = 0x380;
constexpr uint32_t AIRMINI_NCP_TIMEOUT_MS = 5000;
constexpr int64_t AIRMINI_NCP_READBACK_CLOCK_TOLERANCE_MS = 2000;

bool ncp_clock_readback_matches(const std::string &requested,
                                const std::string &returned,
                                uint32_t elapsed_ms) {
    int64_t requested_ms = 0;
    int64_t returned_ms = 0;
    if (!parse_utc_iso8601_ms(requested.c_str(), requested_ms) ||
        !parse_utc_iso8601_ms(returned.c_str(), returned_ms)) {
        return false;
    }

    const int64_t delta_ms = returned_ms - requested_ms;
    const int64_t lower_bound = -AIRMINI_NCP_READBACK_CLOCK_TOLERANCE_MS;
    const int64_t upper_bound =
        static_cast<int64_t>(elapsed_ms) +
        AIRMINI_NCP_READBACK_CLOCK_TOLERANCE_MS;
    return delta_ms >= lower_bound && delta_ms <= upper_bound;
}

}  // namespace

bool CanRpcLink::begin() {
    return rpc_rx_.reserve_initial() && log_rx_.reserve_initial();
}

void CanRpcLink::poll(uint32_t now_ms) {
    if (!application_enabled_) return;
    const DatagramFeedResult rpc_timeout = rpc_rx_.poll(now_ms);
    if (rpc_timeout.status == DatagramStatus::Error) {
        push_link_error(rpc_timeout.error.c_str());
    }
}

bool CanRpcLink::set_physical_enabled(bool enabled) {
    if (enabled == physical_enabled_) return true;

    rpc_rx_.reset();
    log_rx_.reset();
    ncp_clock_rx_.reset();
    link_events_.clear();
    side_events_.clear();

    if (enabled) {
        physical_enabled_ = can_.begin();
        return physical_enabled_;
    }

    cancel("can_disabled");
    physical_enabled_ = false;
    debug_log_rx_requested_ = true;
    return can_.end();
}

void CanRpcLink::poll_physical(uint32_t now_ms) {
    if (!physical_enabled()) return;

    can_.poll();

    if (debug_log_rx_requested_) {
        const DatagramFeedResult log_timeout = log_rx_.poll(now_ms);
        if (log_timeout.status == DatagramStatus::Error) {
            push_side_error(log_timeout.error.c_str());
        }
    }

    drain_rx();
    poll_ncp_clock(now_ms);
    can_.poll();
    poll_debug_log_rx_filter();
}

size_t CanRpcLink::drain_rx() {
    if (!physical_enabled()) return 0;

    size_t drained = 0;
    const uint32_t start_ms = millis();

    for (; drained < AC_CAN_RX_DRAIN_PRESSURE_BUDGET; ++drained) {
        RawCanFrame frame;
        if (!can_.receive(frame, 0)) break;

        handle_frame(frame, millis());
        if (drained + 1 < AC_CAN_RX_DRAIN_BASE_BUDGET) continue;

        CanControllerStatus status;
        const bool pressure = can_.controller_status(status) && status.valid &&
                              status.msgs_to_rx >=
                                  AC_CAN_RX_BACKPRESSURE_WATERMARK;
        if (!pressure) break;
        if (millis() - start_ms >= AC_CAN_RX_DRAIN_PRESSURE_MAX_MS) break;
    }

    if (can_.tx_queue_depth() > 0) can_.poll();
    return drained;
}

RpcLinkSendResult CanRpcLink::send(RpcPayloadView payload) {
    if (!application_enabled_ || !physical_enabled()) {
        return RpcLinkSendResult::Unavailable;
    }

    const size_t frame_count = datagram_frame_count(payload.size());
    if (frame_count > can_.tx_queue_free()) return RpcLinkSendResult::Busy;

    if (!visit_encoded_datagram(
            reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
            enqueue_datagram_frame, this)) {
        return RpcLinkSendResult::Failed;
    }

    return RpcLinkSendResult::Accepted;
}

bool CanRpcLink::take_event(RpcLinkEvent &event) {
    return link_events_.pop(event);
}

void CanRpcLink::reset() {
    cancel("link_reset");
    rpc_rx_.reset();
    ncp_clock_rx_.reset();
    link_events_.clear();
}

void CanRpcLink::set_peer_absence_expected(bool expected) {
    can_.set_peer_absence_expected(expected);
}

RpcApplicationLinkStatus CanRpcLink::status() const {
    RpcApplicationLinkStatus out;
    out.ready = application_enabled_ && physical_enabled();
    out.tx_idle = !physical_enabled() || can_.tx_idle();
    out.tx_queue_depth = can_.tx_queue_depth();
    out.rx_pressure_events = can_.stats().rx_queue_full_alerts;

    CanControllerStatus can_status;
    if (can_.controller_status(can_status) && can_status.valid) {
        out.tx_queue_depth += can_status.msgs_to_tx;
        out.rx_pressure = can_status.msgs_to_rx >=
                          AC_CAN_RX_BACKPRESSURE_WATERMARK;
    }

    return out;
}

void CanRpcLink::set_application_enabled(bool enabled) {
    if (enabled == application_enabled_) return;

    application_enabled_ = enabled;
    if (!enabled) cancel("application_disabled");
    rpc_rx_.reset();
    link_events_.clear();
}

bool CanRpcLink::take_side_event(CanSideEvent &event) {
    return side_events_.pop(event);
}

void CanRpcLink::set_service_frame_observer(
    As11ServiceFrameObserver observer,
    void *context) {
    service_frame_observer_ = observer;
    service_frame_context_ = observer ? context : nullptr;
}

bool CanRpcLink::recover_can(const char *reason) {
    if (!physical_enabled()) return false;

    rpc_rx_.reset();
    log_rx_.reset();
    cancel(reason ? reason : "can_recovery");
    link_events_.clear();

    CanSideEvent event;
    event.kind = CanSideEventKind::ApplicationReset;
    event.detail = reason ? reason : "can_recovery";
    (void)side_events_.push(std::move(event));

    return can_.recover_or_restart(reason);
}

void CanRpcLink::request_debug_log_rx(bool enabled) {
    if (!physical_enabled()) return;
    if (enabled == debug_log_rx_requested_) return;

    debug_log_rx_requested_ = enabled;
    log_rx_.reset();
}

CanQuiesceStatus CanRpcLink::can_quiesce_status() const {
    CanQuiesceStatus out;
    if (!physical_enabled()) {
        out.debug_log_rx_enabled = false;
        return out;
    }

    out.debug_log_rx_enabled = can_.debug_log_rx_enabled();
    out.debug_log_filter_pending =
        debug_log_rx_requested_ != out.debug_log_rx_enabled;
    return out;
}

bool CanRpcLink::enqueue_datagram_frame(void *context,
                                        const DatagramFrame &frame) {
    auto *link = static_cast<CanRpcLink *>(context);
    if (!link) return false;

    RawCanFrame raw;
    raw.id = AC_CAN_TX_ID;
    raw.len = frame.len;
    for (uint8_t i = 0; i < frame.len; ++i) raw.data[i] = frame.data[i];
    return link->can_.enqueue_tx(raw);
}

bool CanRpcLink::enqueue_ncp_frame(void *context,
                                   const DatagramFrame &frame) {
    auto *link = static_cast<CanRpcLink *>(context);
    if (!link) return false;

    RawCanFrame raw;
    raw.id = AIRMINI_NCP_TX_ID;
    raw.len = frame.len;
    for (uint8_t i = 0; i < frame.len; ++i) raw.data[i] = frame.data[i];
    return link->can_.enqueue_tx(raw);
}

void CanRpcLink::handle_frame(const RawCanFrame &frame, uint32_t now_ms) {
    if (frame.extended || frame.remote) return;

    if (frame.id == AC_CAN_RX_ID) {
        if (application_enabled_) handle_application_frame(frame, now_ms);
        return;
    }

    if (frame.id == AIRMINI_NCP_RX_ID) {
        handle_ncp_clock_frame(frame, now_ms);
        return;
    }

    if (frame.id == AC_CAN_LOG_ID) {
        handle_debug_frame(frame, now_ms);
        return;
    }

    if (frame.id == AC_CAN_BOOT_ID) {
        push_boot_notification(frame);
        return;
    }

    if (frame.id == AC_AS11_SERVICE_RX_ID && service_frame_observer_) {
        service_frame_observer_(service_frame_context_, frame, now_ms);
    }
}

void CanRpcLink::handle_application_frame(const RawCanFrame &frame,
                                          uint32_t now_ms) {
    const DatagramFeedResult result = rpc_rx_.feed(frame.data, frame.len,
                                                   now_ms);
    if (result.status == DatagramStatus::Complete) {
        RpcLinkEvent event;
        event.kind = RpcLinkEventKind::Payload;
        event.payload = copy_rpc_payload(result.payload_data,
                                         result.payload_len);
        if (!event.payload || !link_events_.push(std::move(event))) {
            push_link_error("payload_queue_full");
        }
        rpc_rx_.reset();
    } else if (result.status == DatagramStatus::Error) {
        push_link_error(result.error.c_str());
    }
}

void CanRpcLink::handle_debug_frame(const RawCanFrame &frame,
                                    uint32_t now_ms) {
    if (!debug_log_rx_requested_) return;

    const DatagramFeedResult result = log_rx_.feed(frame.data, frame.len,
                                                   now_ms);
    if (result.status == DatagramStatus::Complete) {
        CanSideEvent event;
        event.kind = CanSideEventKind::DebugPayload;
        event.payload = copy_rpc_payload(result.payload_data,
                                         result.payload_len);
        if (!event.payload || !side_events_.push(std::move(event))) {
            push_side_error("payload_queue_full");
        }
        log_rx_.reset();
    } else if (result.status == DatagramStatus::Error) {
        push_side_error(result.error.c_str());
    }
}

bool CanRpcLink::request_write(const char *datetime, uint32_t now_ms) {
    if (!available() || pending() || ncp_clock_result_pending_ || !datetime) {
        return false;
    }

    ncp_clock_requested_datetime_ = datetime;
    ncp_clock_write_started_ms_ = now_ms;
    ncp_clock_phase_ = NcpClockPhase::WaitingSet;
    if (!send_ncp_record(AirMiniNcpClockCommand::Set, datetime, now_ms)) {
        ncp_clock_phase_ = NcpClockPhase::Idle;
        ncp_clock_requested_datetime_.clear();
        ncp_clock_write_started_ms_ = 0;
        return false;
    }
    return true;
}

bool CanRpcLink::take_result(AirMiniNcpClockResult &result) {
    if (!ncp_clock_result_pending_) return false;

    result = std::move(ncp_clock_result_);
    ncp_clock_result_ = {};
    ncp_clock_result_pending_ = false;
    return true;
}

bool CanRpcLink::pending() const {
    return ncp_clock_phase_ != NcpClockPhase::Idle;
}

void CanRpcLink::cancel(const char *reason) {
    if (!pending()) {
        ncp_clock_rx_.reset();
        return;
    }
    finish_ncp_clock(false, reason ? reason : "cancelled");
}

bool CanRpcLink::send_ncp_record(AirMiniNcpClockCommand command,
                                 const char *datetime,
                                 uint32_t now_ms) {
    uint8_t record[32] = {};
    size_t record_size = 0;
    ++ncp_clock_tag_;
    if (ncp_clock_tag_ == 0 || ncp_clock_tag_ == 0xff) ncp_clock_tag_ = 1;
    ncp_clock_expected_tag_ = ncp_clock_tag_;
    if (!encode_airmini_ncp_clock_request(
            command, ncp_clock_expected_tag_, datetime,
            record, sizeof(record), record_size)) {
        return false;
    }

    if (!ncp_clock_rx_.reserve_initial()) return false;

    if (datagram_frame_count(record_size) > can_.tx_queue_free()) return false;

    ncp_clock_rx_.reset();
    if (!visit_encoded_datagram(record, record_size,
                                enqueue_ncp_frame, this)) {
        ncp_clock_rx_.reset();
        return false;
    }

    ncp_clock_deadline_ms_ = now_ms + AIRMINI_NCP_TIMEOUT_MS;
    if (ncp_clock_deadline_ms_ == 0) ncp_clock_deadline_ms_ = 1;
    return true;
}

void CanRpcLink::poll_ncp_clock(uint32_t now_ms) {
    if (!pending()) return;

    const DatagramFeedResult timeout = ncp_clock_rx_.poll(now_ms);
    if (timeout.status == DatagramStatus::Error) {
        finish_ncp_clock(false, timeout.error.c_str());
        return;
    }
    if (ncp_clock_deadline_ms_ != 0 &&
        static_cast<int32_t>(now_ms - ncp_clock_deadline_ms_) >= 0) {
        finish_ncp_clock(false, "response_timeout");
    }
}

void CanRpcLink::handle_ncp_clock_frame(const RawCanFrame &frame,
                                        uint32_t now_ms) {
    if (!pending()) return;

    const DatagramFeedResult result =
        ncp_clock_rx_.feed(frame.data, frame.len, now_ms);
    if (result.status == DatagramStatus::Error) {
        finish_ncp_clock(false, result.error.c_str());
        return;
    }
    if (result.status != DatagramStatus::Complete) return;

    AirMiniNcpClockResponse response;
    if (!decode_airmini_ncp_clock_response(
            reinterpret_cast<const uint8_t *>(result.payload_data),
            result.payload_len, response)) {
        finish_ncp_clock(false, "invalid_response");
        return;
    }
    if (response.tag != ncp_clock_expected_tag_) {
        finish_ncp_clock(false, "response_mismatch");
        return;
    }
    if (response.error) {
        finish_ncp_clock(false, response.error_text.c_str(),
                         response.error_code);
        return;
    }

    const uint8_t expected_command =
        ncp_clock_phase_ == NcpClockPhase::WaitingSet ? 0x85 : 0x84;
    if (response.command != expected_command) {
        finish_ncp_clock(false, "response_mismatch");
        return;
    }

    if (ncp_clock_phase_ == NcpClockPhase::WaitingSet) {
        ncp_clock_phase_ = NcpClockPhase::WaitingWriteReadback;
        if (!send_ncp_record(AirMiniNcpClockCommand::Get, nullptr, now_ms)) {
            finish_ncp_clock(false, "readback_queue_failed");
        }
        return;
    }

    if (ncp_clock_phase_ == NcpClockPhase::WaitingWriteReadback &&
        !ncp_clock_readback_matches(
            ncp_clock_requested_datetime_, response.datetime,
            static_cast<uint32_t>(now_ms - ncp_clock_write_started_ms_))) {
        finish_ncp_clock(false, "readback_mismatch", 0,
                         response.datetime.c_str());
        return;
    }

    finish_ncp_clock(true, nullptr, 0, response.datetime.c_str());
}

void CanRpcLink::finish_ncp_clock(bool succeeded,
                                  const char *reason,
                                  int16_t error_code,
                                  const char *datetime) {
    ncp_clock_result_ = {};
    ncp_clock_result_.succeeded = succeeded;
    ncp_clock_result_.datetime = datetime ? datetime : "";
    ncp_clock_result_.error_code = error_code;
    ncp_clock_result_.reason = reason ? reason : "";
    ncp_clock_result_pending_ = true;
    ncp_clock_phase_ = NcpClockPhase::Idle;
    ncp_clock_deadline_ms_ = 0;
    ncp_clock_write_started_ms_ = 0;
    ncp_clock_expected_tag_ = 0;
    ncp_clock_requested_datetime_.clear();
    ncp_clock_rx_.reset();
}

void CanRpcLink::poll_debug_log_rx_filter() {
    if (can_.debug_log_rx_enabled() == debug_log_rx_requested_) return;
    if (!can_.set_debug_log_rx_enabled(debug_log_rx_requested_)) return;

    log_rx_.reset();
}

void CanRpcLink::push_link_error(const char *detail) {
    const char *error = detail ? detail : "framing_error";
    Log::logf(CAT_CAN, LOG_WARN, "[RPC][FRAMING] %s\n", error);

    RpcLinkEvent event;
    event.kind = RpcLinkEventKind::FramingError;
    event.detail = error;
    (void)link_events_.push(std::move(event));
}

void CanRpcLink::push_side_error(const char *detail) {
    CanSideEvent event;
    event.kind = CanSideEventKind::DebugFramingError;
    event.detail = detail ? detail : "framing_error";
    (void)side_events_.push(std::move(event));
}

void CanRpcLink::push_boot_notification(const RawCanFrame &frame) {
    char id[8];
    snprintf(id, sizeof(id), "%03lX", static_cast<unsigned long>(frame.id));

    CanSideEvent event;
    event.kind = CanSideEventKind::BootNotification;
    event.detail = "FgPowerup 0x";
    event.detail += id;
    event.detail += " [";
    event.detail += std::to_string(frame.len);
    event.detail += "] ";
    event.detail += hex_bytes(frame.data, frame.len);
    (void)side_events_.push(std::move(event));
}

}  // namespace aircannect
