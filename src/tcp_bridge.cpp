#include "tcp_bridge.h"

#include <algorithm>
#include <string.h>
#include <utility>

#include "debug_log.h"

namespace aircannect {

const char *tcp_bridge_client_protocol_name(
    TcpBridgeClientProtocol protocol) {
    switch (protocol) {
        case TcpBridgeClientProtocol::Unknown: return "unknown";
        case TcpBridgeClientProtocol::Rpc: return "rpc";
        case TcpBridgeClientProtocol::Service: return "service";
    }
    return "unknown";
}

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

void TcpBridge::poll(RpcPassthroughPort &rpc,
                     bool service_entry_allowed) {
    if (!started()) return;

    accept_clients();
    poll_service_completion();
    pump_outputs();
    poll_inputs(rpc, service_entry_allowed);
    poll_service_idle(millis());
}

void TcpBridge::broadcast_rpc_payload(const RpcPayloadRef &payload) {
    if (!started() || !payload) return;

    if (!raw_client_connected()) return;

    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i] || !clients_[i].connected()) continue;
        if (protocols_[i] != TcpBridgeClientProtocol::Rpc) continue;

        if (!output_queues_[i].push(payload)) {
            Log::logf(CAT_TCP, LOG_WARN,
                      "[CLIENT %u] outbound queue full; dropping payload\n",
                      static_cast<unsigned>(i));
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
        if (clients_[i] && clients_[i].connected() &&
            protocols_[i] == TcpBridgeClientProtocol::Rpc) {
            return true;
        }
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
        dst.protocol = protocols_[i];
        dst.line_buffer_len = lines_[i].length();
        dst.output_queue_count = output_queues_[i].count();
        if (protocols_[i] == TcpBridgeClientProtocol::Service &&
            service_output_) {
            const size_t total = service_output_->size();
            dst.output_current_len =
                service_output_pos_ < total
                    ? total - service_output_pos_
                    : 0;
        } else if (output_current_[i]) {
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
        protocols_[i] = TcpBridgeClientProtocol::Unknown;
        Log::logf(CAT_TCP, LOG_INFO, "[CLIENT %u] connected from %s\n",
                  static_cast<unsigned>(i),
                  clients_[i].remoteIP().toString().c_str());
        return;
    }

    incoming.println("ERR: max clients");
    incoming.stop();
}

void TcpBridge::poll_service_completion() {
    if (service_owner_ >= AC_MAX_TCP_CLIENTS) return;

    const size_t owner = service_owner_;
    As11ServiceTransactionError error;
    if (service_.take_error(As11ServiceOwner::TcpBridge, error)) {
        Log::logf(CAT_TCP, LOG_DEBUG,
                  "[CLIENT %u SERVICE] closing after error=%s\n",
                  static_cast<unsigned>(owner),
                  as11_service_transaction_error_name(error));
        disconnect_slot(owner);
        return;
    }

    if (!service_output_) {
        (void)service_.take_response(As11ServiceOwner::TcpBridge,
                                     service_output_,
                                     service_close_after_output_);
        service_output_pos_ = 0;
    }
}

void TcpBridge::pump_outputs() {
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (!clients_[i] || !clients_[i].connected()) continue;

        LineOutputPumpResult result;
        if (protocols_[i] == TcpBridgeClientProtocol::Service) {
            result = pump_service_output(i);
        } else if (protocols_[i] == TcpBridgeClientProtocol::Rpc) {
            result = pump_rpc_output(i);
        }

        if (result.fatal_error) {
            disconnect_slot(i);
            continue;
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

    const RpcPayloadView payload = rpc_payload_view(output_current_[idx]);
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
        data = reinterpret_cast<const uint8_t *>(payload.data()) +
               output_pos_[idx];
    }

    result.written = write_line_nonblocking(client, idx, "CLIENT", data,
                                            chunk, result.fatal_error);
    if (result.fatal_error || result.written == 0) return result;

    output_pos_[idx] += result.written;
    if (output_pos_[idx] >= total_len) {
        output_current_[idx].reset();
        output_pos_[idx] = 0;
        result.completed = true;
    }
    return result;
}

LineOutputPumpResult TcpBridge::pump_service_output(size_t idx) {
    LineOutputPumpResult result;
    if (idx >= AC_MAX_TCP_CLIENTS || idx != service_owner_ ||
        !service_output_) {
        return result;
    }

    WiFiClient &client = clients_[idx];
    if (!client || !client.connected()) return result;

    const size_t total = service_output_->size();
    if (service_output_pos_ >= total) {
        service_output_.reset();
        service_output_pos_ = 0;
        result.completed = true;
        if (service_close_after_output_) {
            service_close_after_output_ = false;
            disconnect_slot(idx);
        }
        return result;
    }

    const size_t remaining = total - service_output_pos_;
    const size_t chunk = std::min<size_t>(remaining, AC_TCP_WRITE_CHUNK);
    const uint8_t *data = service_output_->data() + service_output_pos_;

    result.written = write_line_nonblocking(client, idx, "SERVICE", data,
                                            chunk, result.fatal_error);
    if (result.fatal_error || result.written == 0) return result;

    service_output_pos_ += result.written;
    service_last_activity_ms_ = millis();
    if (service_output_pos_ >= total) {
        service_output_.reset();
        service_output_pos_ = 0;
        result.completed = true;
        if (service_close_after_output_) {
            service_close_after_output_ = false;
            disconnect_slot(idx);
        }
    }
    return result;
}

void TcpBridge::poll_inputs(RpcPassthroughPort &rpc,
                            bool service_entry_allowed) {
    for (size_t i = 0; i < AC_MAX_TCP_CLIENTS; ++i) {
        if (clients_[i].fd() < 0) {
            if (protocols_[i] != TcpBridgeClientProtocol::Unknown) {
                disconnect_slot(i);
            }
            continue;
        }

        if (!clients_[i].connected()) {
            Log::logf(CAT_TCP, LOG_INFO, "[CLIENT %u] disconnected\n",
                      static_cast<unsigned>(i));
            disconnect_slot(i);
            continue;
        }

        if (protocols_[i] == TcpBridgeClientProtocol::Service &&
            (service_.pending() || service_output_)) {
            continue;
        }
        if (protocols_[i] == TcpBridgeClientProtocol::Service) {
            (void)pump_service_input(i, service_entry_allowed);
            continue;
        }

        size_t budget = AC_TCP_READ_BYTES_PER_POLL;
        while (budget > 0 && clients_[i].available()) {
            budget--;
            const uint8_t value = static_cast<uint8_t>(clients_[i].read());
            const uint32_t now_ms = millis();

            if (protocols_[i] == TcpBridgeClientProtocol::Unknown) {
                if (value == AS11_SERVICE_PACKET_MAGIC) {
                    if (!begin_service_client(i, now_ms)) break;
                    service_header_[0] = value;
                    service_header_received_ = 1;
                    (void)pump_service_input(i, service_entry_allowed);
                    break;
                } else {
                    protocols_[i] = TcpBridgeClientProtocol::Rpc;
                }
            }

            if (!accept_rpc_byte(i, value, rpc, now_ms)) break;
        }
    }
}

bool TcpBridge::begin_service_client(size_t idx, uint32_t now_ms) {
    if (!service_.available()) {
        Log::logf(CAT_TCP, LOG_INFO,
                  "[CLIENT %u SERVICE] unavailable without CAN\n",
                  static_cast<unsigned>(idx));
        disconnect_slot(idx);
        return false;
    }

    if (service_owner_ < AC_MAX_TCP_CLIENTS && service_owner_ != idx) {
        Log::logf(CAT_TCP, LOG_WARN,
                  "[CLIENT %u SERVICE] rejected; client %u owns service\n",
                  static_cast<unsigned>(idx),
                  static_cast<unsigned>(service_owner_));
        disconnect_slot(idx);
        return false;
    }
    if (!service_.acquire(As11ServiceOwner::TcpBridge)) {
        Log::logf(CAT_TCP, LOG_WARN,
                  "[CLIENT %u SERVICE] rejected; service is in use\n",
                  static_cast<unsigned>(idx));
        disconnect_slot(idx);
        return false;
    }

    service_owner_ = idx;
    protocols_[idx] = TcpBridgeClientProtocol::Service;
    service_last_activity_ms_ = now_ms;
    Log::logf(CAT_TCP, LOG_INFO,
              "[CLIENT %u] protocol=service\n",
              static_cast<unsigned>(idx));
    return true;
}

bool TcpBridge::pump_service_input(size_t idx,
                                   bool service_entry_allowed) {
    if (idx >= AC_MAX_TCP_CLIENTS || idx != service_owner_ ||
        service_.pending() || service_output_) {
        return false;
    }

    WiFiClient &client = clients_[idx];
    if (!client || !client.connected()) return false;

    size_t budget = AS11_SERVICE_PACKET_MAX_BYTES;
    while (budget > 0 && client.available()) {
        uint8_t *destination = nullptr;
        size_t remaining = 0;
        if (service_header_received_ < AS11_SERVICE_PACKET_HEADER_BYTES) {
            destination = service_header_ + service_header_received_;
            remaining = AS11_SERVICE_PACKET_HEADER_BYTES -
                        service_header_received_;
        } else {
            if (!service_request_ ||
                service_request_received_ >= service_request_->size()) {
                disconnect_slot(idx);
                return false;
            }

            destination = service_request_->data() +
                          service_request_received_;
            remaining = service_request_->size() -
                        service_request_received_;
        }

        const size_t available = static_cast<size_t>(client.available());
        const size_t chunk = std::min(remaining,
                                      std::min(available, budget));

        const int received = client.read(destination, chunk);
        if (received < 0) {
            Log::logf(CAT_TCP, LOG_WARN,
                      "[CLIENT %u SERVICE] request read failed\n",
                      static_cast<unsigned>(idx));
            disconnect_slot(idx);
            return false;
        }
        if (received == 0) return true;

        const size_t received_size = static_cast<size_t>(received);
        budget -= received_size;
        service_last_activity_ms_ = millis();

        if (service_header_received_ < AS11_SERVICE_PACKET_HEADER_BYTES) {
            service_header_received_ += received_size;
            if (service_header_received_ <
                AS11_SERVICE_PACKET_HEADER_BYTES) {
                continue;
            }

            size_t packet_size = 0;
            const As11ServicePacketError packet_error =
                as11_service_packet_size_from_header(
                    service_header_, service_header_received_, packet_size);
            if (packet_error != As11ServicePacketError::None) {
                Log::logf(CAT_TCP, LOG_WARN,
                          "[CLIENT %u SERVICE] invalid request error=%s\n",
                          static_cast<unsigned>(idx),
                          as11_service_packet_error_name(packet_error));
                disconnect_slot(idx);
                return false;
            }

            service_request_ = LargeByteBuffer::allocate(packet_size);
            if (!service_request_) {
                Log::logf(
                    CAT_TCP, LOG_WARN,
                    "[CLIENT %u SERVICE] request allocation failed size=%u\n",
                    static_cast<unsigned>(idx),
                    static_cast<unsigned>(packet_size));
                disconnect_slot(idx);
                return false;
            }

            memcpy(service_request_->data(), service_header_,
                   AS11_SERVICE_PACKET_HEADER_BYTES);
            service_request_received_ =
                AS11_SERVICE_PACKET_HEADER_BYTES;
            continue;
        }

        service_request_received_ += received_size;
        if (service_request_received_ < service_request_->size()) continue;

        std::unique_ptr<LargeByteBuffer> complete =
            std::move(service_request_);
        reset_service_request();

        if (!service_.submit_packet(As11ServiceOwner::TcpBridge,
                                    std::move(complete),
                                    service_entry_allowed, millis())) {
            Log::logf(CAT_TCP, LOG_WARN,
                      "[CLIENT %u SERVICE] request rejected error=%s\n",
                      static_cast<unsigned>(idx),
                      as11_service_transaction_error_name(
                          service_.last_error()));
            disconnect_slot(idx);
            return false;
        }
        return true;
    }
    return true;
}

bool TcpBridge::accept_rpc_byte(size_t idx, uint8_t value,
                                RpcPassthroughPort &rpc,
                                uint32_t now_ms) {
    if (value != '\n') {
        if (value == '\r') return true;
        if (lines_[idx].length() < AC_TCP_LINE_MAX) {
            lines_[idx] += static_cast<char>(value);
            return true;
        }

        release_line_string(lines_[idx]);
        Log::logf(CAT_TCP, LOG_WARN,
                  "[CLIENT %u] line too long; dropping partial payload\n",
                  static_cast<unsigned>(idx));
        return true;
    }

    String line = std::move(lines_[idx]);
    line.trim();
    if (!line.length()) return true;

    const std::string payload(line.c_str());
    if (Log::get_cat_level(CAT_TCP) >= LOG_DEBUG) {
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "[CLIENT %u -> RPC] ",
                 static_cast<unsigned>(idx));
        Log::log_payload(CAT_TCP, LOG_DEBUG, prefix, payload);
    }

    if (!rpc.submit_raw_payload(payload, RpcSource::Tcp)) {
        Log::logf(CAT_TCP, LOG_WARN,
                  "[CLIENT %u] CAN queue rejected payload\n",
                  static_cast<unsigned>(idx));
    } else if (raw_request_observer_) {
        raw_request_observer_(raw_request_observer_context_, payload.data(),
                              payload.size(), now_ms);
    }
    return true;
}

void TcpBridge::poll_service_idle(uint32_t now_ms) {
    if (service_owner_ >= AC_MAX_TCP_CLIENTS) return;

    const size_t owner = service_owner_;
    if (service_.pending() || service_output_) return;
    if (static_cast<uint32_t>(now_ms - service_last_activity_ms_) <
        AC_AS11_SERVICE_TCP_IDLE_TIMEOUT_MS) {
        return;
    }

    Log::logf(CAT_TCP, LOG_INFO,
              "[CLIENT %u SERVICE] idle timeout\n",
              static_cast<unsigned>(owner));
    disconnect_slot(owner);
}

void TcpBridge::reset_service_request() {
    service_request_.reset();
    memset(service_header_, 0, sizeof(service_header_));
    service_header_received_ = 0;
    service_request_received_ = 0;
}

void TcpBridge::disconnect_slot(size_t idx) {
    if (idx >= AC_MAX_TCP_CLIENTS) return;

    if (service_owner_ == idx) {
        if (service_.pending()) {
            Log::logf(CAT_TCP, LOG_DEBUG,
                      "[CLIENT %u SERVICE] transaction cancelled on "
                      "disconnect\n",
                      static_cast<unsigned>(idx));
        }

        service_.release(As11ServiceOwner::TcpBridge);
        reset_service_request();
        service_output_.reset();
        service_output_pos_ = 0;
        service_close_after_output_ = false;
        service_last_activity_ms_ = 0;
        service_owner_ = AC_MAX_TCP_CLIENTS;
    }

    clients_[idx].stop();
    protocols_[idx] = TcpBridgeClientProtocol::Unknown;
    release_line_string(lines_[idx]);
    output_queues_[idx].clear();
    output_current_[idx].reset();
    output_pos_[idx] = 0;
}

}  // namespace aircannect
