#include "rpc_quiesce_coordinator.h"

#include "board_can.h"
#include "debug_log.h"
#include "event_broker.h"
#include "rpc_arbiter.h"
#include "stream_broker.h"

namespace aircannect {

RpcQuiesceCoordinator::RpcQuiesceCoordinator(RpcArbiter &transport,
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

    if (!timed_out(now_ms) || timeout_logged_) return;
    timeout_logged_ = true;
    log_timeout();
}

bool RpcQuiesceCoordinator::complete() const {
    if (!requested_) return true;
    return streams_.quiesced() && events_.quiesced() &&
           transport_.quiesce_idle();
}

bool RpcQuiesceCoordinator::timed_out(uint32_t now_ms) const {
    return requested_ && deadline_ms_ &&
           static_cast<int32_t>(now_ms - deadline_ms_) >= 0;
}

bool RpcQuiesceCoordinator::reboot_allowed(uint32_t now_ms) const {
    return complete() || timed_out(now_ms);
}

void RpcQuiesceCoordinator::begin(uint32_t now_ms) {
    requested_ = true;
    timeout_logged_ = false;
    deadline_ms_ = now_ms + AC_ESP_OTA_QUIESCE_TIMEOUT_MS;

    Log::logf(CAT_OTA, LOG_INFO,
              "quiescing AS11 push traffic before ESP OTA\n");

    transport_.set_quiesce_mode(true);
    streams_.request_quiesce(now_ms);
    events_.request_quiesce(now_ms);
}

void RpcQuiesceCoordinator::end(uint32_t now_ms) {
    requested_ = false;
    timeout_logged_ = false;
    deadline_ms_ = 0;

    streams_.clear_quiesce();
    events_.clear_quiesce(now_ms);
    transport_.set_quiesce_mode(false);
}

void RpcQuiesceCoordinator::log_timeout() {
    const RpcRuntimeStatus transport = transport_.runtime_status();
    const EventBrokerStatus events = events_.status();

    Log::logf(CAT_OTA, LOG_WARN,
              "AS11 quiesce timed out stream=%u event=%u pending=%u "
              "retry=%u queue=%u tx_q=%u event_active=%u "
              "event_pending=%u\n",
              streams_.quiesced() ? 1u : 0u,
              events_.quiesced() ? 1u : 0u,
              transport.pending_request_id ? 1u : 0u,
              transport.dispatch_retry_id ? 1u : 0u,
              static_cast<unsigned>(transport.request_queue_depth),
              static_cast<unsigned>(transport_.can_driver().tx_queue_depth()),
              events.subscription_active ? 1u : 0u,
              events.subscribe_pending ? 1u : 0u);
}

}  // namespace aircannect
