#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <stdint.h>
#include <string>

#include "as11_service_manager.h"
#include "board.h"
#include "fixed_queue.h"
#include "large_byte_buffer.h"
#include "line_protocol_server.h"
#include "rpc_transport_ports.h"

namespace aircannect {

using TcpRawRequestObserver = void (*)(void *context,
                                       const char *payload,
                                       size_t payload_len,
                                       uint32_t now_ms);

enum class TcpBridgeClientProtocol : uint8_t {
    Unknown,
    Rpc,
    Service,
};

const char *tcp_bridge_client_protocol_name(TcpBridgeClientProtocol protocol);

struct TcpBridgeClientStatus {
    bool connected = false;
    IPAddress remote_ip;
    TcpBridgeClientProtocol protocol = TcpBridgeClientProtocol::Unknown;
    size_t line_buffer_len = 0;
    size_t output_queue_count = 0;
    size_t output_current_len = 0;
};

class TcpBridge : private LineProtocolServerBase {
public:
    explicit TcpBridge(As11ServiceManager &service) : service_(service) {}

    // Lifecycle
    bool begin(uint16_t port = AC_TCP_BRIDGE_PORT);
    bool restart(uint16_t port = AC_TCP_BRIDGE_PORT);
    void stop();
    void poll(RpcPassthroughPort &rpc, bool service_entry_allowed);

    // RPC transport
    void broadcast_rpc_payload(const RpcPayloadRef &payload);
    void set_raw_request_observer(TcpRawRequestObserver observer,
                                  void *context);

    // Status
    int connected_count();
    bool raw_client_connected();
    bool started() const { return line_server_started(); }
    uint16_t port() const { return line_server_port(); }
    size_t client_statuses(TcpBridgeClientStatus *out, size_t max);

private:
    void accept_clients();
    void poll_service_completion();
    void pump_outputs();
    LineOutputPumpResult pump_rpc_output(size_t idx);
    LineOutputPumpResult pump_service_output(size_t idx);
    void poll_inputs(RpcPassthroughPort &rpc,
                     bool service_entry_allowed);
    bool begin_service_client(size_t idx, uint32_t now_ms);
    bool pump_service_input(size_t idx, bool service_entry_allowed);
    bool accept_rpc_byte(size_t idx, uint8_t value,
                         RpcPassthroughPort &rpc, uint32_t now_ms);
    void poll_service_idle(uint32_t now_ms);
    void reset_service_request();
    void disconnect_slot(size_t idx);

    As11ServiceManager &service_;
    WiFiClient clients_[AC_MAX_TCP_CLIENTS];
    TcpBridgeClientProtocol protocols_[AC_MAX_TCP_CLIENTS] = {};
    String lines_[AC_MAX_TCP_CLIENTS];

    FixedQueue<RpcPayloadRef, AC_TCP_TX_QUEUE_DEPTH>
        output_queues_[AC_MAX_TCP_CLIENTS];
    RpcPayloadRef output_current_[AC_MAX_TCP_CLIENTS];
    size_t output_pos_[AC_MAX_TCP_CLIENTS] = {};

    uint8_t service_header_[AS11_SERVICE_PACKET_HEADER_BYTES] = {};
    size_t service_header_received_ = 0;
    std::unique_ptr<LargeByteBuffer> service_request_;
    size_t service_request_received_ = 0;
    std::shared_ptr<const LargeByteBuffer> service_output_;
    size_t service_output_pos_ = 0;
    bool service_close_after_output_ = false;
    uint32_t service_last_activity_ms_ = 0;
    size_t service_owner_ = AC_MAX_TCP_CLIENTS;

    TcpRawRequestObserver raw_request_observer_ = nullptr;
    void *raw_request_observer_context_ = nullptr;
};

}  // namespace aircannect
