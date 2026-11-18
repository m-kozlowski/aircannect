#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <stdint.h>
#include <string>

#include "as11_service_manager.h"
#include "board.h"
#include "can_rpc_link.h"
#include "fixed_queue.h"
#include "large_byte_buffer.h"
#include "line_protocol_server.h"
#include "rpc_transport_ports.h"

namespace aircannect {

enum class TcpBridgeClientProtocol : uint8_t {
    Unknown,
    Rpc,
    Service,
    Modem,
};

class TcpBridge : private LineProtocolServerBase {
public:
    TcpBridge(As11ServiceManager &service, CanRpcLink &can)
        : service_(service), can_(can) {}

    // Lifecycle
    bool begin(uint16_t port = AC_TCP_BRIDGE_PORT);
    bool restart(uint16_t port = AC_TCP_BRIDGE_PORT);
    void stop();
    void poll(RpcPassthroughPort &rpc, bool service_entry_allowed);

    // RPC transport
    void broadcast_rpc_payload(const RpcPayloadRef &payload);

    // Status
    bool raw_client_connected();
    bool started() const { return line_server_started(); }
    uint16_t port() const { return line_server_port(); }

private:
    struct ServiceClient {
        bool prepare_busy_reply();

        uint8_t header[AS11_SERVICE_PACKET_HEADER_BYTES] = {};
        size_t received = 0;
        std::unique_ptr<LargeByteBuffer> request;
        std::shared_ptr<const LargeByteBuffer> output;
        size_t output_pos = 0;
        bool close_after_output = false;
        uint32_t last_activity_ms = 0;
    };

    void accept_clients();
    void poll_service_completion();
    void pump_outputs();
    LineOutputPumpResult pump_payload_output(size_t idx, bool append_lf);
    LineOutputPumpResult pump_service_output(size_t idx);
    void poll_inputs(RpcPassthroughPort &rpc,
                     bool service_entry_allowed);
    bool begin_service_client(size_t idx, uint32_t now_ms);
    bool pump_service_input(size_t idx, bool service_entry_allowed);
    bool accept_rpc_byte(size_t idx, uint8_t value,
                         RpcPassthroughPort &rpc);
    void poll_service_idle(uint32_t now_ms);
    void disconnect_slot(size_t idx);

    // Modem lines and channel ownership
    bool select_protocol(size_t idx, uint8_t value, RpcPassthroughPort &rpc);
    bool begin_modem_client(size_t idx);
    bool accept_modem_byte(size_t idx, uint8_t value);
    bool send_modem_line();
    void release_modem_channel();
    static void receive_modem(void *context, const DatagramFeedResult &result);

    As11ServiceManager &service_;
    CanRpcLink &can_;
    WiFiClient clients_[AC_MAX_TCP_CLIENTS];
    TcpBridgeClientProtocol protocols_[AC_MAX_TCP_CLIENTS] = {};
    String lines_[AC_MAX_TCP_CLIENTS];

    FixedQueue<RpcPayloadRef, AC_TCP_TX_QUEUE_DEPTH>
        output_queues_[AC_MAX_TCP_CLIENTS];
    RpcPayloadRef output_current_[AC_MAX_TCP_CLIENTS];
    size_t output_pos_[AC_MAX_TCP_CLIENTS] = {};

    ServiceClient service_clients_[AC_MAX_TCP_CLIENTS];
    size_t service_owner_ = AC_MAX_TCP_CLIENTS;

    std::unique_ptr<LargeByteBuffer> modem_line_;
    size_t modem_line_size_ = 0;
    size_t modem_owner_ = AC_MAX_TCP_CLIENTS;
    bool modem_line_ready_ = false;
    bool modem_needs_off_ = false;

};

}  // namespace aircannect
