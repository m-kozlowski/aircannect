#include "can_rpc_link.h"

#include <stdio.h>

#include "board.h"
#include "debug_log.h"
#include "hex_util.h"

namespace aircannect {

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
    link_events_.clear();
    side_events_.clear();

    if (enabled) {
        physical_enabled_ = can_.begin();
        return physical_enabled_;
    }

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
    rpc_rx_.reset();
    link_events_.clear();
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

void CanRpcLink::handle_frame(const RawCanFrame &frame, uint32_t now_ms) {
    if (frame.extended || frame.remote) return;

    if (frame.id == AC_CAN_RX_ID) {
        if (application_enabled_) handle_application_frame(frame, now_ms);
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
