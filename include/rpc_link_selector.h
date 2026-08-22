#pragma once

#include "as11_transport.h"
#include "rpc_application_link.h"

namespace aircannect {

class RpcLinkSelector final : public RpcApplicationLink {
public:
    RpcLinkSelector(RpcApplicationLink &can_link,
                    RpcApplicationLink &ble_link)
        : can_link_(can_link), ble_link_(ble_link) {}

    bool begin() override;
    void poll(uint32_t now_ms) override;
    RpcLinkSendResult send(RpcPayloadView payload) override;
    bool take_event(RpcLinkEvent &event) override;
    void reset() override;
    void set_peer_absence_expected(bool expected) override;
    void set_controlled_disconnect(bool requested) override;
    bool controlled_disconnect_complete() const override;

    RpcApplicationLinkStatus status() const override;
    const char *name() const override;

    bool select(As11Transport transport);
    As11Transport selected() const { return selected_; }
    bool can_selected() const { return selected_ == As11Transport::Can; }

private:
    RpcApplicationLink &active_link();
    const RpcApplicationLink &active_link() const;

    RpcApplicationLink &can_link_;
    RpcApplicationLink &ble_link_;
    As11Transport selected_ = As11Transport::Can;
    bool can_started_ = false;
    bool ble_started_ = false;
    bool switch_event_pending_ = false;
};

}  // namespace aircannect
