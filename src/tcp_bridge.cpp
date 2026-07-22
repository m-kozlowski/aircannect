#include "tcp_bridge.h"

#include <utility>

#include "debug_log.h"

namespace aircannect {

bool TcpBridge::begin(uint16_t port) {
    return begin_line_server(port, "BRIDGE");
}

bool TcpBridge::restart(uint16_t port) {
    stop();
    return begin(port);
}

void TcpBridge::stop() {
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        disconnect_slot(i);
    }
    stop_line_server();
}

void TcpBridge::poll(RpcArbiter &arbiter) {
    if (!started()) return;
    accept_clients();
    pump_outputs();
    poll_inputs(arbiter);
}

void TcpBridge::broadcast_rpc_payload(const RpcPayloadRef &payload) {
    if (!started() || !payload) return;
    stats_.broadcasts++;

    if (!raw_client_connected()) return;

    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i] || !clients_[i].connected()) continue;
        if (!output_queues_[i].push(payload)) {
            stats_.queue_drops++;
            Log::logf(CAT_TCP, LOG_WARN,
                      "[CLIENT %u] outbound queue full; dropping payload\n",
                      static_cast<unsigned>(i));
        } else {
        }
    }
}

void TcpBridge::set_raw_request_observer(TcpRawRequestObserver observer,
                                         void *context) {
    raw_request_observer_ = observer;
    raw_request_observer_context_ = context;
}

int TcpBridge::connected_count() {
    int count = 0;
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (clients_[i] && clients_[i].connected()) count++;
    }
    return count;
}

bool TcpBridge::raw_client_connected() {
    if (!started()) return false;
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (clients_[i] && clients_[i].connected()) return true;
    }
    return false;
}

size_t TcpBridge::client_statuses(TcpBridgeClientStatus *out, size_t max) {
    if (!out || max == 0) return 0;
    const size_t count = max < AC_MAX_TCP_CLIENTS ? max : AC_MAX_TCP_CLIENTS;
    for (size_t i = 0; i < count; ++i) {
        TcpBridgeClientStatus &dst = out[i];
        dst = TcpBridgeClientStatus();
        dst.connected = clients_[i] && clients_[i].connected();
        if (!dst.connected) continue;
        dst.remote_ip = clients_[i].remoteIP();
        dst.line_buffer_len = lines_[i].length();
        dst.output_queue_count = output_queues_[i].count();
        if (output_current_[i]) {
            const size_t total = output_current_[i]->size() + 1;
            dst.output_current_len =
                output_pos_[i] < total ? total - output_pos_[i] : 0;
        }
    }
    return count;
}

void TcpBridge::accept_clients() {
    WiFiClient incoming = accept_line_client();
    if (!incoming) return;

    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (clients_[i] && clients_[i].connected()) continue;
        disconnect_slot(i);
        clients_[i] = incoming;
        stats_.accepted_clients++;
        Log::logf(CAT_TCP, LOG_INFO, "[CLIENT %u] connected from %s\n",
                  static_cast<unsigned>(i),
                  clients_[i].remoteIP().toString().c_str());
        return;
    }

    stats_.rejected_clients++;
    incoming.println("ERR: max clients");
    incoming.stop();
}

void TcpBridge::pump_outputs() {
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i] || !clients_[i].connected()) continue;

        LineOutputPumpResult result = pump_rpc_output(i);
        if (result.fatal_error) {
            stats_.disconnected_clients++;
            disconnect_slot(i);
            continue;
        }

        if (result.completed) {
            stats_.lines_out++;
        }
    }
}

LineOutputPumpResult TcpBridge::pump_rpc_output(size_t idx) {
    LineOutputPumpResult result;
    if (idx >= AC_MAX_TCP_CLIENTS) return result;
    WiFiClient &client = clients_[idx];
    if (!client || !client.connected()) return result;

    if (!output_current_[idx]) {
        RpcPayloadRef next;
        if (!output_queues_[idx].pop(next)) return result;
        output_current_[idx] = std::move(next);
        output_pos_[idx] = 0;
    }
    if (!output_current_[idx]) return result;

    const std::string &payload = *output_current_[idx];
    const size_t payload_len = payload.size();
    const size_t total_len = payload_len + 1;
    if (output_pos_[idx] >= total_len) {
        output_current_[idx].reset();
        output_pos_[idx] = 0;
        result.completed = true;
        return result;
    }

    const uint8_t newline = '\n';
    const uint8_t *data = &newline;
    size_t chunk = 1;
    if (output_pos_[idx] < payload_len) {
        const size_t remaining = payload_len - output_pos_[idx];
        chunk = remaining < AC_TCP_WRITE_CHUNK
                    ? remaining
                    : AC_TCP_WRITE_CHUNK;
        data = reinterpret_cast<const uint8_t *>(
            payload.data() + output_pos_[idx]);
    }

    result.written = write_line_nonblocking(client, idx, "CLIENT", data,
                                            chunk, result.fatal_error);
    if (result.fatal_error || result.written == 0) return result;

    output_pos_[idx] += result.written;
    note_line_bytes_out(result.written);
    if (output_pos_[idx] >= total_len) {
        output_current_[idx].reset();
        output_pos_[idx] = 0;
        result.completed = true;
    }
    return result;
}

void TcpBridge::poll_inputs(RpcArbiter &arbiter) {
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i]) continue;

        if (!clients_[i].connected()) {
            Log::logf(CAT_TCP, LOG_INFO, "[CLIENT %u] disconnected\n",
                      static_cast<unsigned>(i));
            stats_.disconnected_clients++;
            disconnect_slot(i);
            continue;
        }

        size_t budget = AC_TCP_READ_BYTES_PER_POLL;
        while (budget > 0 && clients_[i].available()) {
            budget--;
            char c = static_cast<char>(clients_[i].read());
            stats_.bytes_in++;
            if (c == '\n') {
                String line = lines_[i];
                line.trim();
                lines_[i] = "";
                if (!line.length()) continue;

                stats_.lines_in++;
                const std::string payload(line.c_str());
                if (Log::get_cat_level(CAT_TCP) >= LOG_DEBUG) {
                    char prefix[32];
                    snprintf(prefix, sizeof(prefix), "[CLIENT %u -> RPC] ",
                             static_cast<unsigned>(i));
                    Log::log_payload(CAT_TCP, LOG_DEBUG, prefix, payload);
                }
                if (!arbiter.submit_raw_payload(payload, RpcSource::Tcp)) {
                    stats_.enqueue_failures++;
                    Log::logf(CAT_TCP, LOG_WARN,
                              "[CLIENT %u] CAN queue rejected payload\n",
                              static_cast<unsigned>(i));
                } else if (raw_request_observer_) {
                    raw_request_observer_(raw_request_observer_context_,
                                          payload.data(), payload.size(),
                                          millis());
                }
            } else if (c != '\r') {
                if (lines_[i].length() < AC_TCP_LINE_MAX) {
                    lines_[i] += c;
                } else {
                    stats_.overlong_lines++;
                    lines_[i] = "";
                    Log::logf(CAT_TCP, LOG_WARN,
                              "[CLIENT %u] line too long; dropping partial payload\n",
                              static_cast<unsigned>(i));
                }
            }
        }
    }
}

void TcpBridge::disconnect_slot(size_t idx) {
    if (idx >= AC_MAX_TCP_CLIENTS) return;
    if (clients_[idx]) clients_[idx].stop();
    lines_[idx] = "";
    output_queues_[idx].clear();
    output_current_[idx].reset();
    output_pos_[idx] = 0;
}

}  // namespace aircannect
