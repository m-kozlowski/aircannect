#include "rpc_link_selector.h"

namespace aircannect {

bool RpcLinkSelector::begin() {
    if (!can_started_) can_started_ = can_link_.begin();
    if (selected_ == As11Transport::Ble && !ble_started_) {
        ble_started_ = ble_link_.begin();
    }
    return can_started_ &&
           (selected_ != As11Transport::Ble || ble_started_);
}

void RpcLinkSelector::poll(uint32_t now_ms) {
    active_link().poll(now_ms);
}

RpcLinkSendResult RpcLinkSelector::send(RpcPayloadView payload) {
    return active_link().send(payload);
}

bool RpcLinkSelector::take_event(RpcLinkEvent &event) {
    if (switch_event_pending_) {
        switch_event_pending_ = false;
        event = {};
        event.kind = RpcLinkEventKind::TransportChanged;
        event.detail = "AS11 transport changed";
        return true;
    }
    return active_link().take_event(event);
}

void RpcLinkSelector::reset() { active_link().reset(); }

void RpcLinkSelector::set_peer_absence_expected(bool expected) {
    active_link().set_peer_absence_expected(expected);
}

void RpcLinkSelector::set_controlled_disconnect(bool requested) {
    active_link().set_controlled_disconnect(requested);
}

bool RpcLinkSelector::controlled_disconnect_complete() const {
    return active_link().controlled_disconnect_complete();
}

RpcApplicationLinkStatus RpcLinkSelector::status() const {
    return active_link().status();
}

const char *RpcLinkSelector::name() const { return active_link().name(); }

bool RpcLinkSelector::select(As11Transport transport) {
    if (!as11_transport_valid(transport)) return false;
    if (transport == As11Transport::Ble && !ble_started_) {
        ble_started_ = ble_link_.begin();
    }
    if (transport == selected_) {
        return transport != As11Transport::Ble || ble_started_;
    }

    active_link().set_peer_absence_expected(false);
    active_link().reset();
    selected_ = transport;
    switch_event_pending_ = true;
    return transport != As11Transport::Ble || ble_started_;
}

RpcApplicationLink &RpcLinkSelector::active_link() {
    return selected_ == As11Transport::Ble ? ble_link_ : can_link_;
}

const RpcApplicationLink &RpcLinkSelector::active_link() const {
    return selected_ == As11Transport::Ble ? ble_link_ : can_link_;
}

}  // namespace aircannect
