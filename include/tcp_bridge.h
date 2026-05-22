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
    bool begin(uint16_t port = AC_TCP_BRIDGE_PORT);
    bool restart(uint16_t port = AC_TCP_BRIDGE_PORT);
    void stop();
    void poll(RpcArbiter &arbiter);

    void broadcast_rpc_payload(const std::string &payload);

    int connected_count();
    bool started() const { return line_server_started(); }
    uint16_t port() const { return line_server_port(); }
    const TcpBridgeStats &stats() const { return stats_; }
    const LineProtocolIoStats &io_stats() const { return line_io_stats(); }
    size_t client_statuses(TcpBridgeClientStatus *out, size_t max);

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

    TcpBridgeStats stats_ = {};
};

}  // namespace aircannect
