#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "fixed_queue.h"
#include "line_protocol_server.h"
#include "rpc_arbiter.h"

namespace aircannect {

struct TcpBridgeStats {
    uint32_t accepted_clients = 0;
    uint32_t disconnected_clients = 0;
    uint32_t broadcasts = 0;
    uint32_t broadcast_targets = 0;
    uint32_t broadcasts_without_clients = 0;
    uint32_t lines_in = 0;
    uint32_t bytes_in = 0;
    uint32_t input_yields = 0;
    uint32_t lines_out = 0;
    uint32_t overlong_lines = 0;
    uint32_t enqueue_failures = 0;
    uint32_t queue_drops = 0;
    uint32_t rejected_clients = 0;
};

class TcpBridge : private LineProtocolServerBase {
public:
    bool begin(uint16_t port = AC_TCP_BRIDGE_PORT);
    bool restart(uint16_t port = AC_TCP_BRIDGE_PORT);
    void stop();
    void poll(RpcArbiter &arbiter);

    void broadcast_rpc_payload(const std::string &payload);

    void print_stats(Print &out);
    void print_status(Print &out);
    int connected_count();
    bool started() const { return line_server_started(); }
    uint16_t port() const { return line_server_port(); }

private:
    void accept_clients();
    void pump_outputs();
    void poll_inputs(RpcArbiter &arbiter);
    void disconnect_slot(size_t idx);

    WiFiClient clients_[AC_MAX_TCP_CLIENTS];
    String lines_[AC_MAX_TCP_CLIENTS];

    FixedQueue<String, AC_TCP_TX_QUEUE_DEPTH> output_queues_[AC_MAX_TCP_CLIENTS];
    String output_current_[AC_MAX_TCP_CLIENTS];
    size_t output_pos_[AC_MAX_TCP_CLIENTS] = {};

    uint32_t last_line_in_ms_ = 0;
    uint32_t last_line_out_ms_ = 0;
    TcpBridgeStats stats_ = {};
};

}  // namespace aircannect
