#include "tcp_bridge.h"

#include <algorithm>
#include <string.h>
#include <utility>

#include "debug_log.h"

namespace aircannect {
namespace {

constexpr char MODEM_ON[] = "AT-PASSTHROUGH-ON\r\n";
constexpr char MODEM_OFF[] = "AT-PASSTHROUGH-OFF\r\n";
// A complete datagram must fit the existing atomic CAN enqueue operation.
constexpr size_t MODEM_LINE_MAX = std::min<size_t>(
    AC_TCP_LINE_MAX, 3 + 7 * (AC_CAN_TX_QUEUE_DEPTH - 1));

}  // namespace

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
    if (modem_owner_ < AC_MAX_TCP_CLIENTS &&
        (!can_.modem_available() || service_.exclusive_requested())) {
        disconnect_slot(modem_owner_);
    }

    release_modem_channel();
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
            result = pump_payload_output(i, true);
        } else if (protocols_[i] == TcpBridgeClientProtocol::Modem) {
            result = pump_payload_output(i, false);
        }

        if (result.fatal_error) {
            disconnect_slot(i);
            continue;
        }
    }
}

LineOutputPumpResult TcpBridge::pump_payload_output(size_t idx, bool append_lf) {
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
    const size_t total_len = payload_len + (append_lf ? 1 : 0);
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

        if (protocols_[i] == TcpBridgeClientProtocol::Modem &&
            modem_line_ready_ && !send_modem_line()) {
            continue;
        }

        size_t budget = AC_TCP_READ_BYTES_PER_POLL;
        while (budget > 0 && clients_[i].available()) {
            budget--;
            const uint8_t value = static_cast<uint8_t>(clients_[i].read());
            if (protocols_[i] == TcpBridgeClientProtocol::Unknown) {
                if (!lines_[i].length() && value == AS11_SERVICE_PACKET_MAGIC) {
                    if (!begin_service_client(i, millis())) break;
                    service_header_[0] = value;
                    service_header_received_ = 1;
                    (void)pump_service_input(i, service_entry_allowed);
                    break;
                }

                if (!select_protocol(i, value, rpc)) break;
            } else if (protocols_[i] == TcpBridgeClientProtocol::Modem) {
                if (!accept_modem_byte(i, value)) break;
            } else {
                if (!accept_rpc_byte(i, value, rpc)) break;
            }
        }
    }
}

bool TcpBridge::select_protocol(size_t idx, uint8_t value,
                                RpcPassthroughPort &rpc) {
    const size_t matched = lines_[idx].length();
    if (value == static_cast<uint8_t>(MODEM_ON[matched])) {
        if (!lines_[idx].concat(static_cast<char>(value))) {
            disconnect_slot(idx);
            return false;
        }
        if (matched + 1 < sizeof(MODEM_ON) - 1) return true;

        release_line_string(lines_[idx]);
        return begin_modem_client(idx);
    }

    // A nonmatching prefix remains ordinary RPC input, including its CRs.
    String prefix = std::move(lines_[idx]);
    protocols_[idx] = TcpBridgeClientProtocol::Rpc;
    for (size_t i = 0; i < prefix.length(); ++i) {
        if (!accept_rpc_byte(idx, prefix.c_str()[i], rpc)) return false;
    }
    return accept_rpc_byte(idx, value, rpc);
}

bool TcpBridge::begin_modem_client(size_t idx) {
    if (modem_owner_ < AC_MAX_TCP_CLIENTS || modem_needs_off_ ||
        !can_.modem_available() || service_.exclusive_requested()) {
        disconnect_slot(idx);
        return false;
    }

    modem_line_ = LargeByteBuffer::allocate(MODEM_LINE_MAX);
    if (!modem_line_ || !can_.set_modem_observer(receive_modem, this)) {
        modem_line_.reset();
        disconnect_slot(idx);
        return false;
    }

    modem_owner_ = idx;
    protocols_[idx] = TcpBridgeClientProtocol::Modem;
    memcpy(modem_line_->data(), MODEM_ON, sizeof(MODEM_ON) - 1);
    modem_line_size_ = sizeof(MODEM_ON) - 1;
    modem_line_ready_ = true;
    return send_modem_line();
}

bool TcpBridge::accept_modem_byte(size_t idx, uint8_t value) {
    if (modem_line_size_ == modem_line_->size()) {
        Log::logf(CAT_TCP, LOG_WARN, "[CLIENT %u MODEM] line too long\n",
                  static_cast<unsigned>(idx));
        disconnect_slot(idx);
        return false;
    }

    uint8_t *data = modem_line_->data();
    data[modem_line_size_++] = value;
    modem_line_ready_ = modem_line_size_ >= 2 && value == '\n' &&
                        data[modem_line_size_ - 2] == '\r';
    return !modem_line_ready_ || send_modem_line();
}

bool TcpBridge::send_modem_line() {
    const char *data = reinterpret_cast<const char *>(modem_line_->data());
    const RpcLinkSendResult result = can_.send_modem(
        RpcPayloadView(data, modem_line_size_));

    if (result == RpcLinkSendResult::Busy) return false;

    const bool on = modem_line_size_ == sizeof(MODEM_ON) - 1 &&
                    memcmp(data, MODEM_ON, modem_line_size_) == 0;
    const bool off = modem_line_size_ == sizeof(MODEM_OFF) - 1 &&
                     memcmp(data, MODEM_OFF, modem_line_size_) == 0;
    if (on) modem_needs_off_ = true;

    if (result != RpcLinkSendResult::Accepted) {
        Log::logf(CAT_TCP, LOG_WARN, "[MODEM] CAN send failed; closing TCP\n");
        disconnect_slot(modem_owner_);
        return false;
    }

    if (off) modem_needs_off_ = false;
    modem_line_size_ = 0;
    modem_line_ready_ = false;
    return true;
}

void TcpBridge::release_modem_channel() {
    if (modem_owner_ < AC_MAX_TCP_CLIENTS || !modem_needs_off_) return;

    // Keep OFF pending across CAN backpressure, even when the server is stopped.
    if (can_.send_modem(RpcPayloadView(MODEM_OFF, sizeof(MODEM_OFF) - 1)) ==
        RpcLinkSendResult::Accepted) {
        modem_needs_off_ = false;
    }
}

void TcpBridge::receive_modem(void *context, const DatagramFeedResult &result) {
    auto &self = *static_cast<TcpBridge *>(context);
    const size_t owner = self.modem_owner_;
    if (owner >= AC_MAX_TCP_CLIENTS) return;
    if (result.status == DatagramStatus::Complete && result.payload_len == 0) {
        return;
    }

    RpcPayloadRef payload;
    if (result.status == DatagramStatus::Complete) {
        payload = copy_rpc_payload(result.payload_data, result.payload_len);
    }

    if (!payload || !self.output_queues_[owner].push(std::move(payload))) {
        Log::logf(CAT_TCP, LOG_WARN,
                  "[MODEM] response lost; closing TCP error=%s\n",
                  result.status == DatagramStatus::Error
                      ? result.error.c_str() : "output_queue_full_or_no_memory");
        self.disconnect_slot(owner);
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
                                RpcPassthroughPort &rpc) {
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
                  "[CLIENT %u] RPC transport rejected payload\n",
                  static_cast<unsigned>(idx));
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

    if (modem_owner_ == idx) {
        (void)can_.set_modem_observer(nullptr, nullptr);
        modem_owner_ = AC_MAX_TCP_CLIENTS;
        modem_line_.reset();
        modem_line_size_ = 0;
        modem_line_ready_ = false;
        release_modem_channel();
    }

    if (service_owner_ == idx) {
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
