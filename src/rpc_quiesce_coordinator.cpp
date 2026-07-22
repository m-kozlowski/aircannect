#include "rpc_quiesce_coordinator.h"

#include "board_can.h"
#include "debug_log.h"
#include "event_broker.h"
#include "stream_broker.h"

namespace aircannect {

RpcQuiesceCoordinator::RpcQuiesceCoordinator(RpcQuiescePort &transport,
                                             EventBroker &events,
                                             StreamBroker &streams)
    : transport_(transport), events_(events), streams_(streams) {}

void RpcQuiesceCoordinator::update(bool requested, uint32_t now_ms) {
    if (requested != requested_) {
        if (requested) {
            begin(now_ms);
        } else {
            end(now_ms);
        }
    }

    if (!requested_ || complete_ || timed_out_) return;

    RpcQuiesceStatus transport = transport_.quiesce_status();
    if (push_traffic_quiesced(transport)) {
        transport_.request_debug_log_rx(false);
        transport = transport_.quiesce_status();
    }

    if (push_traffic_quiesced(transport) &&
        !transport.debug_log_rx_enabled &&
        !transport.debug_log_filter_pending) {
        complete_ = true;
        deadline_ms_ = 0;
        return;
    }

    if (!deadline_ms_ ||
        static_cast<int32_t>(now_ms - deadline_ms_) < 0) {
        return;
    }

    timed_out_ = true;
    log_timeout();
}

bool RpcQuiesceCoordinator::complete() const {
    return !requested_ || complete_;
}

bool RpcQuiesceCoordinator::timed_out() const {
    return requested_ && timed_out_;
}

bool RpcQuiesceCoordinator::reboot_allowed() const {
    return complete() || timed_out();
}

void RpcQuiesceCoordinator::begin(uint32_t now_ms) {
    requested_ = true;
    complete_ = false;
    timed_out_ = false;
    deadline_ms_ = now_ms + AC_ESP_OTA_QUIESCE_TIMEOUT_MS;
    if (deadline_ms_ == 0) deadline_ms_ = 1;

    Log::logf(CAT_OTA, LOG_INFO,
              "quiescing AS11 push traffic before ESP OTA\n");

    transport_.set_quiesce_mode(true);
    streams_.request_quiesce(now_ms);
    events_.request_quiesce(now_ms);
}

void RpcQuiesceCoordinator::end(uint32_t now_ms) {
    requested_ = false;
    complete_ = false;
    timed_out_ = false;
    deadline_ms_ = 0;

    transport_.request_debug_log_rx(true);
    transport_.set_quiesce_mode(false);
    streams_.clear_quiesce();
    events_.clear_quiesce(now_ms);
}

bool RpcQuiesceCoordinator::push_traffic_quiesced(
    const RpcQuiesceStatus &transport) const {
    return streams_.quiesced() && events_.quiesced() && transport.idle;
}

void RpcQuiesceCoordinator::log_timeout() {
    const RpcQuiesceStatus transport = transport_.quiesce_status();
    const EventBrokerStatus events = events_.status();

    Log::logf(CAT_OTA, LOG_WARN,
              "AS11 quiesce timed out stream=%u event=%u pending=%u "
              "retry=%u queue=%u payload_q=%u tx_q=%u debug_rx=%u "
              "filter_pending=%u event_active=%u event_pending=%u\n",
              streams_.quiesced() ? 1u : 0u,
              events_.quiesced() ? 1u : 0u,
              transport.pending_request ? 1u : 0u,
              transport.dispatch_retry ? 1u : 0u,
              static_cast<unsigned>(transport.request_queue_depth),
              static_cast<unsigned>(transport.payload_queue_depth),
              static_cast<unsigned>(transport.tx_queue_depth),
              transport.debug_log_rx_enabled ? 1u : 0u,
              transport.debug_log_filter_pending ? 1u : 0u,
              events.subscription_active ? 1u : 0u,
              events.subscribe_pending ? 1u : 0u);
}

}  // namespace aircannect
