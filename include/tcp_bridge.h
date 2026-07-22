#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "fixed_queue.h"
#include "line_protocol_server.h"
#include "rpc_transport_ports.h"

namespace aircannect {

using TcpRawRequestObserver = void (*)(void *context,
                                       const char *payload,
                                       size_t payload_len,
                                       uint32_t now_ms);

struct TcpBridgeStats {
    uint32_t accepted_clients = 0;
    uint32_t disconnected_clients = 0;
    uint32_t broadcasts = 0;
    uint32_t lines_in = 0;
    uint32_t bytes_in = 0;
    uint32_t lines_out = 0;
    uint32_t overlong_lines = 0;
    uint32_t enqueue_failures = 0;
    uint32_t queue_drops = 0;
    uint32_t rejected_clients = 0;
};

struct TcpBridgeClientStatus {
    bool connected = false;
    IPAddress remote_ip;
    size_t line_buffer_len = 0;
    size_t output_queue_count = 0;
    size_t output_current_len = 0;
};

class TcpBridge : private LineProtocolServerBase {
public:
    // Lifecycle
    bool begin(uint16_t port = AC_TCP_BRIDGE_PORT);
    bool restart(uint16_t port = AC_TCP_BRIDGE_PORT);
    void stop();
    void poll(RpcPassthroughPort &rpc);

    // RPC transport
    void broadcast_rpc_payload(const RpcPayloadRef &payload);
    void set_raw_request_observer(TcpRawRequestObserver observer,
                                  void *context);

    // Status
    int connected_count();
    bool raw_client_connected();
    bool started() const { return line_server_started(); }
    uint16_t port() const { return line_server_port(); }
    const TcpBridgeStats &stats() const { return stats_; }
    size_t client_statuses(TcpBridgeClientStatus *out, size_t max);

private:
    void accept_clients();
    void pump_outputs();
    LineOutputPumpResult pump_rpc_output(size_t idx);
    void poll_inputs(RpcPassthroughPort &rpc);
    void disconnect_slot(size_t idx);

    WiFiClient clients_[AC_MAX_TCP_CLIENTS];
    String lines_[AC_MAX_TCP_CLIENTS];

    FixedQueue<RpcPayloadRef, AC_TCP_TX_QUEUE_DEPTH>
        output_queues_[AC_MAX_TCP_CLIENTS];
    RpcPayloadRef output_current_[AC_MAX_TCP_CLIENTS];
    size_t output_pos_[AC_MAX_TCP_CLIENTS] = {};

    TcpRawRequestObserver raw_request_observer_ = nullptr;
    void *raw_request_observer_context_ = nullptr;

    TcpBridgeStats stats_ = {};
};

}  // namespace aircannect
