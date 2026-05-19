#include "tcp_bridge.h"

#include "debug_log.h"

namespace aircannect {

bool TcpBridge::begin(uint16_t port) {
    return begin_line_server(port, "TCP");
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

void TcpBridge::broadcast_rpc_payload(const std::string &payload) {
    if (!started()) return;
    stats_.broadcasts++;

    bool has_client = false;
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (clients_[i] && clients_[i].connected()) {
            has_client = true;
            break;
        }
    }
    if (!has_client) {
        stats_.broadcasts_without_clients++;
        return;
    }

    String line(payload.c_str());
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i] || !clients_[i].connected()) continue;
        if (!output_queues_[i].push(line)) {
            stats_.queue_drops++;
            Log::logf(CAT_TCP, LOG_WARN,
                      "[TCP %u] outbound queue full; dropping payload\n",
                      static_cast<unsigned>(i));
        } else {
            stats_.broadcast_targets++;
        }
    }
}

void TcpBridge::print_stats(Print &out) {
    out.print(" tcp_started=");
    out.print(started() ? "yes" : "no");
    out.print(" tcp_port=");
    out.print(port());
    out.print(" tcp_clients=");
    out.print(connected_count());
    out.print(" tcp_accepted=");
    out.print(stats_.accepted_clients);
    out.print(" tcp_disconnected=");
    out.print(stats_.disconnected_clients);
    out.print(" tcp_broadcasts=");
    out.print(stats_.broadcasts);
    out.print(" tcp_broadcast_targets=");
    out.print(stats_.broadcast_targets);
    out.print(" tcp_broadcast_no_clients=");
    out.print(stats_.broadcasts_without_clients);
    out.print(" tcp_in=");
    out.print(stats_.lines_in);
    out.print(" tcp_bytes_in=");
    out.print(stats_.bytes_in);
    out.print(" tcp_input_yields=");
    out.print(stats_.input_yields);
    out.print(" tcp_out=");
    out.print(stats_.lines_out);
    out.print(" tcp_bytes_out=");
    out.print(line_io_stats().bytes_out);
    out.print(" tcp_write_attempts=");
    out.print(line_io_stats().write_attempts);
    out.print(" tcp_write_deferred=");
    out.print(line_io_stats().write_deferred);
    out.print(" tcp_write_zero=");
    out.print(line_io_stats().write_zero);
    out.print(" tcp_write_errors=");
    out.print(line_io_stats().write_errors);
    out.print(" tcp_overlong=");
    out.print(stats_.overlong_lines);
    out.print(" tcp_enqueue_fail=");
    out.print(stats_.enqueue_failures);
    out.print(" tcp_q_drops=");
    out.print(stats_.queue_drops);
    out.print(" tcp_rejected=");
    out.print(stats_.rejected_clients);
    out.print(" tcp_last_in_ms=");
    out.print(last_line_in_ms_);
    out.print(" tcp_last_out_ms=");
    out.print(last_line_out_ms_);
}

void TcpBridge::print_status(Print &out) {
    out.print("[TCP] started=");
    out.print(started() ? "yes" : "no");
    out.print(" port=");
    out.print(port());
    out.print(" clients=");
    out.println(connected_count());
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i] || !clients_[i].connected()) continue;
        out.print("[TCP ");
        out.print(i);
        out.print("] remote=");
        out.print(clients_[i].remoteIP());
        out.print(" line_buf=");
        out.print(lines_[i].length());
        out.print(" out_q=");
        out.print(output_queues_[i].count());
        out.print(" out_current=");
        out.println(output_current_[i].length());
    }
}

int TcpBridge::connected_count() {
    int count = 0;
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (clients_[i] && clients_[i].connected()) count++;
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
        Log::logf(CAT_TCP, LOG_INFO, "[TCP %u] connected from %s\n",
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

        LineOutputPumpResult result = pump_line_output(
            clients_[i], output_queues_[i], output_current_[i], output_pos_[i],
            i, "TCP", true);
        if (result.fatal_error) {
            stats_.disconnected_clients++;
            disconnect_slot(i);
            continue;
        }

        if (result.completed) {
            stats_.lines_out++;
            last_line_out_ms_ = millis();
        }
    }
}

void TcpBridge::poll_inputs(RpcArbiter &arbiter) {
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i]) continue;

        if (!clients_[i].connected()) {
            Log::logf(CAT_TCP, LOG_INFO, "[TCP %u] disconnected\n",
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
                last_line_in_ms_ = millis();
                const std::string payload(line.c_str());
                if (Log::get_cat_level(CAT_TCP) >= LOG_DEBUG) {
                    char prefix[32];
                    snprintf(prefix, sizeof(prefix), "[TCP %u -> RPC] ",
                             static_cast<unsigned>(i));
                    Log::log_payload(CAT_TCP, LOG_DEBUG, prefix, payload);
                }
                if (!arbiter.submit_raw_payload(payload, RpcSource::Tcp)) {
                    stats_.enqueue_failures++;
                    Log::logf(CAT_TCP, LOG_WARN,
                              "[TCP %u] CAN queue rejected payload\n",
                              static_cast<unsigned>(i));
                }
            } else if (c != '\r') {
                if (lines_[i].length() < AC_TCP_LINE_MAX) {
                    lines_[i] += c;
                } else {
                    stats_.overlong_lines++;
                    lines_[i] = "";
                    Log::logf(CAT_TCP, LOG_WARN,
                              "[TCP %u] line too long; dropping partial payload\n",
                              static_cast<unsigned>(i));
                }
            }
        }
        if (budget == 0 && clients_[i].available()) stats_.input_yields++;
    }
}

void TcpBridge::disconnect_slot(size_t idx) {
    if (idx >= AC_MAX_TCP_CLIENTS) return;
    if (clients_[idx]) clients_[idx].stop();
    lines_[idx] = "";
    output_queues_[idx].clear();
    output_current_[idx] = "";
    output_pos_[idx] = 0;
}

}  // namespace aircannect
