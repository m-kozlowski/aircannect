#include "as11_ble_rpc_link.h"

#include <ArduinoJson.h>
#include <strings.h>

#include "debug_log.h"

namespace aircannect {

namespace {

static constexpr const char *AS11_BLE_SERVICE_UUID =
    "0000fd56-0000-1000-8000-00805f9b34fb";
static constexpr const char *AS11_BLE_TX_UUID =
    "a6220002-35f1-4b20-afae-cb089d2044aa";
static constexpr const char *AS11_BLE_RX_UUID =
    "a6220003-35f1-4b20-afae-cb089d2044aa";

bool copy_text(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) return false;
    if (!value) value = "";
    if (strlen(value) >= out_size) return false;

    strcpy(out, value);
    return true;
}

}  // namespace

#if AC_BLE_ENABLED
class As11BleClientCallbacks : public NimBLEClientCallbacks {
public:
    explicit As11BleClientCallbacks(As11BleRpcLink *owner) : owner_(owner) {}

    void onDisconnect(NimBLEClient *client, int reason) override {
        (void)client;
        if (owner_) owner_->note_disconnected(reason);
    }

private:
    As11BleRpcLink *owner_ = nullptr;
};

class As11BleScanCallbacks : public NimBLEScanCallbacks {
public:
    explicit As11BleScanCallbacks(As11BleRpcLink *owner) : owner_(owner) {}

    void onResult(const NimBLEAdvertisedDevice *device) override {
        if (owner_) owner_->note_scan_result(device);
    }

private:
    As11BleRpcLink *owner_ = nullptr;
};
#endif

const char *as11_ble_link_state_name(As11BleLinkState state) {
    switch (state) {
        case As11BleLinkState::Disabled: return "disabled";
        case As11BleLinkState::MissingCredentials:
            return "missing_credentials";
        case As11BleLinkState::WaitingForScanner:
            return "waiting_for_scanner";
        case As11BleLinkState::Scanning: return "scanning";
        case As11BleLinkState::Connecting: return "connecting";
        case As11BleLinkState::Discovering: return "discovering";
        case As11BleLinkState::Authenticating: return "authenticating";
        case As11BleLinkState::Ready: return "ready";
        case As11BleLinkState::Backoff: return "backoff";
    }
    return "unknown";
}

bool As11BleRpcLink::begin() {
#if AC_BLE_ENABLED
    if (!requests_.begin() || !notifications_.begin() || !events_.begin() ||
        !fig_.begin()) {
        return false;
    }

    portENTER_CRITICAL(&mux_);
    if (task_started_) {
        portEXIT_CRITICAL(&mux_);
        return true;
    }
    task_started_ = true;
    portEXIT_CRITICAL(&mux_);

    const BaseType_t created = xTaskCreatePinnedToCore(
        task_entry, "as11_ble", AC_AS11_BLE_TASK_STACK, this,
        AC_AS11_BLE_TASK_PRIO, &task_, 0);
    if (created == pdPASS) return true;

    portENTER_CRITICAL(&mux_);
    task_started_ = false;
    task_ = nullptr;
    portEXIT_CRITICAL(&mux_);
    return false;
#else
    return false;
#endif
}

void As11BleRpcLink::poll(uint32_t now_ms) { (void)now_ms; }

RpcLinkSendResult As11BleRpcLink::send(RpcPayloadView payload) {
    const As11BleLinkStatus snapshot = ble_status();
    if (!snapshot.enabled || !snapshot.authenticated) {
        return RpcLinkSendResult::Unavailable;
    }

    Request request;
    request.payload = copy_rpc_payload(payload.data(), payload.size());
    if (!request.payload) return RpcLinkSendResult::Failed;

#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    queued_requests_++;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif

    if (!requests_.push(std::move(request))) {
#if AC_BLE_ENABLED
        portENTER_CRITICAL(&mux_);
#endif
        queued_requests_--;
#if AC_BLE_ENABLED
        portEXIT_CRITICAL(&mux_);
#endif
        return RpcLinkSendResult::Busy;
    }
    return RpcLinkSendResult::Accepted;
}

bool As11BleRpcLink::take_event(RpcLinkEvent &event) {
    return events_.pop(event);
}

void As11BleRpcLink::reset() {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    reset_requested_ = true;
    portEXIT_CRITICAL(&mux_);
#endif
    requests_.clear();
    events_.clear();
}

RpcApplicationLinkStatus As11BleRpcLink::status() const {
    const As11BleLinkStatus snapshot = ble_status();
    uint8_t queued_requests = 0;
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    queued_requests = queued_requests_;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif

    RpcApplicationLinkStatus out;
    out.ready = snapshot.authenticated;
    out.tx_idle = queued_requests == 0;
    out.tx_queue_depth = queued_requests;
    out.rx_pressure_events = snapshot.notification_drops;
    return out;
}

void As11BleRpcLink::configure(bool enabled,
                               const char *runtime_name,
                               const char *address,
                               const char *client_id,
                               const char *master_key_hex) {
    Configuration next;
    next.enabled = enabled;
    if (!copy_text(next.runtime_name, sizeof(next.runtime_name),
                   runtime_name) ||
        !copy_text(next.address, sizeof(next.address), address) ||
        !copy_text(next.client_id, sizeof(next.client_id), client_id) ||
        !copy_text(next.master_key_hex, sizeof(next.master_key_hex),
                   master_key_hex)) {
        next = {};
        next.enabled = enabled;
    }

#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    next.generation = config_.generation + 1;
    if (next.generation == 0) next.generation = 1;
    config_ = next;
    status_.enabled = enabled;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

As11BleLinkStatus As11BleRpcLink::ble_status() const {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    const As11BleLinkStatus out = status_;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    return out;
}

void As11BleRpcLink::task_entry(void *context) {
    auto *link = static_cast<As11BleRpcLink *>(context);
    if (link) link->task_loop();
    vTaskDelete(nullptr);
}

void As11BleRpcLink::task_loop() {
#if AC_BLE_ENABLED
    As11BleClientCallbacks client_callbacks(this);
    uint32_t applied_generation = 0;
    uint32_t next_attempt_ms = 0;
    uint32_t reconnect_delay_ms = AC_AS11_BLE_RECONNECT_MIN_MS;
    bool ready_seen = false;

    while (true) {
        const Configuration config = configuration();
        const uint32_t now_ms = millis();

        if (!config.enabled) {
            if (client_) disconnect_and_clear();
            set_status(As11BleLinkState::Disabled);
            applied_generation = config.generation;
            ready_seen = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (config.generation != applied_generation || reset_requested()) {
            disconnect_and_clear();
            clear_reset_request();
            applied_generation = config.generation;
            next_attempt_ms = 0;
            reconnect_delay_ms = AC_AS11_BLE_RECONNECT_MIN_MS;
            ready_seen = false;
        }

        if (!config.address[0] || !config.client_id[0] ||
            strlen(config.master_key_hex) != AS11_BLE_KEY_HEX_BYTES) {
            set_status(As11BleLinkState::MissingCredentials,
                       "address_or_credentials_missing");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!client_ || !client_->isConnected()) {
            if (ready_seen) {
                RpcLinkEvent event;
                event.kind = RpcLinkEventKind::Disconnected;
                event.detail = "ble_disconnected";
                (void)events_.push(std::move(event));
                ready_seen = false;
            }
            disconnect_and_clear();

            if (static_cast<int32_t>(now_ms - next_attempt_ms) < 0) {
                set_status(As11BleLinkState::Backoff);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            if (!runtime_.ensure_started(config.runtime_name) ||
                !scan_and_connect(config, client_callbacks) ||
                !discover_pipe() ||
                !authenticate(config)) {
                disconnect_and_clear();
                next_attempt_ms = millis() + reconnect_delay_ms;
                reconnect_delay_ms =
                    reconnect_delay_ms < AC_AS11_BLE_RECONNECT_MAX_MS / 2
                        ? reconnect_delay_ms * 2
                        : AC_AS11_BLE_RECONNECT_MAX_MS;
                set_status(As11BleLinkState::Backoff);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            reconnect_delay_ms = AC_AS11_BLE_RECONNECT_MIN_MS;
            ready_seen = true;
#if AC_BLE_ENABLED
            portENTER_CRITICAL(&mux_);
            status_.reconnects++;
            portEXIT_CRITICAL(&mux_);
#endif
            set_status(As11BleLinkState::Ready);
            set_connected(true, client_->getRssi());
            set_authenticated(true);
            Log::logf(CAT_RPC, LOG_INFO,
                      "[BLE] AS11 session ready address=%s rssi=%d\n",
                      config.address, client_->getRssi());
        }

        drain_notifications(true);

        Request request;
        if (requests_.pop(request) && request.payload) {
            const RpcPayloadView plaintext = rpc_payload_view(request.payload);
            std::unique_ptr<LargeByteBuffer> encrypted =
                crypto_.encrypt(plaintext);
            if (!encrypted ||
                !write_fig(AS11_BLE_VCID_ENCRYPTED_REQUEST,
                           encrypted->data(), encrypted->size())) {
                publish_error("ble_write_failed");
                if (client_->isConnected()) client_->disconnect();
            }

            portENTER_CRITICAL(&mux_);
            if (queued_requests_ > 0) queued_requests_--;
            portEXIT_CRITICAL(&mux_);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
#endif
}

void As11BleRpcLink::disconnect_and_clear() {
#if AC_BLE_ENABLED
    if (client_ && client_->isConnected()) client_->disconnect();
#endif
    release_client();
    crypto_.clear();
    fig_.reset();
    notifications_.clear(pdMS_TO_TICKS(100));
    requests_.clear(pdMS_TO_TICKS(100));
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    queued_requests_ = 0;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    set_connected(false);
    set_authenticated(false);
}

void As11BleRpcLink::release_client() {
#if AC_BLE_ENABLED
    tx_ = nullptr;
    rx_ = nullptr;
    if (!client_ || client_->isConnected()) return;

    client_->deleteServices();
    if (NimBLEDevice::deleteClient(client_)) client_ = nullptr;
#endif
}

void As11BleRpcLink::note_scan_result(const NimBLEAdvertisedDevice *device) {
#if AC_BLE_ENABLED
    if (!device) return;

    const std::string address = device->getAddress().toString();
    portENTER_CRITICAL(&mux_);
    if (strcasecmp(address.c_str(), config_.address) == 0) {
        scan_match_found_ = true;
        scan_match_type_ = device->getAddress().getType();
        scan_match_rssi_ = device->getRSSI();
        copy_text(scan_match_address_, sizeof(scan_match_address_),
                  address.c_str());
    }
    portEXIT_CRITICAL(&mux_);
#else
    (void)device;
#endif
}

void As11BleRpcLink::note_disconnected(int reason) {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    status_.connected = false;
    status_.authenticated = false;
    portEXIT_CRITICAL(&mux_);
    Log::logf(CAT_RPC, LOG_DEBUG,
              "[BLE] AS11 disconnected reason=%d\n", reason);
#else
    (void)reason;
#endif
}

void As11BleRpcLink::note_notification(const uint8_t *data, size_t length) {
    RpcPayloadRef payload = copy_rpc_payload(data, length);
    if (payload && notifications_.push(std::move(payload))) return;

#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    status_.notification_drops++;
    portEXIT_CRITICAL(&mux_);
#endif
}

bool As11BleRpcLink::scan_and_connect(
    const Configuration &config,
    As11BleClientCallbacks &client_callbacks) {
#if AC_BLE_ENABLED
    set_status(As11BleLinkState::WaitingForScanner);
    BleRuntime::ScanLease scan_lease =
        runtime_.acquire_scan(pdMS_TO_TICKS(500));
    if (!scan_lease) return false;

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan) {
        set_status(As11BleLinkState::Backoff, "scanner_unavailable");
        return false;
    }

    portENTER_CRITICAL(&mux_);
    scan_match_found_ = false;
    scan_match_address_[0] = 0;
    portEXIT_CRITICAL(&mux_);

    As11BleScanCallbacks callbacks(this);
    set_status(As11BleLinkState::Scanning);
    scan->clearResults();
    scan->setScanCallbacks(&callbacks, false);
    scan->setMaxResults(0);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    (void)scan->getResults(AC_AS11_BLE_SCAN_MS, false);
    scan->setScanCallbacks(nullptr, false);
    scan->clearResults();

    portENTER_CRITICAL(&mux_);
    const bool found = scan_match_found_;
    const uint8_t address_type = scan_match_type_;
    const int rssi = scan_match_rssi_;
    char address[sizeof(scan_match_address_)] = {};
    copy_text(address, sizeof(address), scan_match_address_);
    portEXIT_CRITICAL(&mux_);
    scan_lease.release();

    if (!found) {
        set_status(As11BleLinkState::Backoff, "device_not_found");
        return false;
    }

    if (!client_) {
        client_ = NimBLEDevice::createClient();
        if (!client_) {
            set_status(As11BleLinkState::Backoff,
                       "client_allocation_failed");
            return false;
        }
    }

    client_->setClientCallbacks(&client_callbacks, false);
    client_->setConnectTimeout(AC_AS11_BLE_CONNECT_TIMEOUT_MS);
    set_status(As11BleLinkState::Connecting);

    const NimBLEAddress peer(std::string(address), address_type);
    if (!client_->connect(peer)) {
        set_status(As11BleLinkState::Backoff, "connect_failed");
        return false;
    }

    set_connected(true, rssi);
    return true;
#else
    (void)config;
    (void)client_callbacks;
    return false;
#endif
}

bool As11BleRpcLink::discover_pipe() {
#if AC_BLE_ENABLED
    if (!client_ || !client_->isConnected()) return false;

    set_status(As11BleLinkState::Discovering);
    NimBLERemoteService *service = client_->getService(AS11_BLE_SERVICE_UUID);
    if (!service) {
        set_status(As11BleLinkState::Backoff, "service_missing");
        return false;
    }

    tx_ = service->getCharacteristic(AS11_BLE_TX_UUID);
    rx_ = service->getCharacteristic(AS11_BLE_RX_UUID);
    if (!tx_ || !rx_ || !tx_->canWrite() || !rx_->canNotify()) {
        set_status(As11BleLinkState::Backoff, "gatt_pipe_missing");
        return false;
    }

    const bool subscribed = rx_->subscribe(
        true,
        [this](NimBLERemoteCharacteristic *characteristic,
               uint8_t *data,
               size_t length,
               bool notify) {
            (void)characteristic;
            (void)notify;
            note_notification(data, length);
        },
        true);
    if (!subscribed) {
        set_status(As11BleLinkState::Backoff, "notification_subscribe_failed");
    }
    return subscribed;
#else
    return false;
#endif
}

bool As11BleRpcLink::authenticate(const Configuration &config) {
    set_status(As11BleLinkState::Authenticating);
    if (!crypto_.set_master_key_hex(config.master_key_hex)) {
        set_status(As11BleLinkState::Backoff, "master_key_invalid");
        return false;
    }

    char challenge[AS11_BLE_KEY_HEX_BYTES + 1] = {};
    char nonce[AS11_BLE_KEY_HEX_BYTES + 1] = {};
    if (!send_plain_request("RequestSession", "clientId", config.client_id,
                            1) ||
        !wait_plain_response(1, challenge, sizeof(challenge), nonce,
                             sizeof(nonce))) {
        set_status(As11BleLinkState::Backoff, "session_request_failed");
        return false;
    }

    char response[AS11_BLE_KEY_HEX_BYTES + 1] = {};
    if (!crypto_.integrity_response_hex(challenge, response) ||
        !send_plain_request("CheckSessionIntegrity", "response", response,
                            2) ||
        !wait_plain_response(2, nullptr, 0, nullptr, 0) ||
        !crypto_.derive_session_key(nonce)) {
        set_status(As11BleLinkState::Backoff, "session_integrity_failed");
        return false;
    }
    return true;
}

bool As11BleRpcLink::send_plain_request(const char *method,
                                        const char *key,
                                        const char *value,
                                        uint32_t id) {
    JsonDocument document;
    document["jsonrpc"] = "2.0";
    document["method"] = method;
    document["id"] = id;
    document["params"][key] = value;

    char json[512];
    const size_t length = serializeJson(document, json, sizeof(json));
    if (length == 0 || length >= sizeof(json)) return false;
    return write_fig(AS11_BLE_VCID_PLAINTEXT_REQUEST,
                     reinterpret_cast<const uint8_t *>(json), length);
}

bool As11BleRpcLink::wait_plain_response(uint32_t id,
                                         char *challenge,
                                         size_t challenge_size,
                                         char *nonce,
                                         size_t nonce_size) {
    const uint32_t deadline = millis() + AC_AS11_BLE_SESSION_TIMEOUT_MS;
    while (static_cast<int32_t>(millis() - deadline) < 0) {
#if AC_BLE_ENABLED
        if (!client_ || !client_->isConnected()) return false;
#endif

        RpcPayloadRef notification;
        if (!notifications_.pop(notification)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (!fig_.feed(notification->data(), notification->size())) {
            return false;
        }

        while (true) {
            As11BleFigPacket packet;
            const As11BleFigDecodeState state = fig_.take(packet);
            if (state == As11BleFigDecodeState::NeedMore) break;
            if (state != As11BleFigDecodeState::Packet) return false;
            if (packet.vcid != AS11_BLE_VCID_PLAINTEXT_RESPONSE ||
                !packet.payload) {
                continue;
            }

            JsonDocument document;
            const DeserializationError error = deserializeJson(
                document, packet.payload->data(), packet.payload->size());
            if (error || document["id"].as<uint32_t>() != id) continue;
            if (document["error"].is<JsonObject>()) return false;

            if (challenge && challenge_size != 0) {
                const char *value = document["result"]["challenge"] | "";
                if (!copy_text(challenge, challenge_size, value)) return false;
            }
            if (nonce && nonce_size != 0) {
                const char *value = document["result"]["nonce"] | "";
                if (!copy_text(nonce, nonce_size, value)) return false;
            }
            return (!challenge || challenge[0]) && (!nonce || nonce[0]);
        }
    }
    return false;
}

bool As11BleRpcLink::write_fig(uint16_t vcid,
                               const uint8_t *data,
                               size_t length) {
#if AC_BLE_ENABLED
    if (!client_ || !client_->isConnected() || !tx_) return false;

    std::unique_ptr<LargeByteBuffer> packet =
        As11BleFigCodec::encode(vcid, data, length);
    if (!packet) return false;

    size_t chunk_size = client_->getMTU();
    chunk_size = chunk_size > 3 ? chunk_size - 3 : 20;
    for (size_t offset = 0; offset < packet->size();
         offset += chunk_size) {
        const size_t remaining = packet->size() - offset;
        const size_t count = remaining < chunk_size ? remaining : chunk_size;
        if (!tx_->writeValue(packet->data() + offset, count, true)) {
            return false;
        }
    }
    return true;
#else
    (void)vcid;
    (void)data;
    (void)length;
    return false;
#endif
}

void As11BleRpcLink::drain_notifications(bool publish_application) {
    RpcPayloadRef notification;
    while (notifications_.pop(notification)) {
        if (!fig_.feed(notification->data(), notification->size())) {
            publish_error("fig_buffer_unavailable");
            continue;
        }

        while (true) {
            As11BleFigPacket packet;
            const As11BleFigDecodeState state = fig_.take(packet);
            if (state == As11BleFigDecodeState::NeedMore) break;
            if (state != As11BleFigDecodeState::Packet) {
#if AC_BLE_ENABLED
                portENTER_CRITICAL(&mux_);
                status_.framing_errors++;
                portEXIT_CRITICAL(&mux_);
#endif
                publish_error("fig_decode_error");
                continue;
            }
            if (publish_application) publish_packet(packet);
        }
    }
}

void As11BleRpcLink::publish_packet(const As11BleFigPacket &packet) {
    if (!packet.payload) return;

    RpcPayloadRef payload;
    if (packet.vcid == AS11_BLE_VCID_ENCRYPTED_RESPONSE) {
        const char *error = nullptr;
        payload = crypto_.decrypt(packet.payload->data(),
                                  packet.payload->size(), error);
        if (!payload) {
            publish_error(error && error[0] ? error : "decrypt_failed");
            return;
        }
    } else if (packet.vcid == AS11_BLE_VCID_PLAINTEXT_RESPONSE) {
        payload = packet.payload;
    } else {
        publish_error("unexpected_vcid");
        return;
    }

    RpcLinkEvent event;
    event.kind = RpcLinkEventKind::Payload;
    event.payload = std::move(payload);
    if (!events_.push(std::move(event))) publish_error("event_queue_full");
}

void As11BleRpcLink::publish_error(const char *detail) {
    RpcLinkEvent event;
    event.kind = RpcLinkEventKind::FramingError;
    event.detail = detail ? detail : "ble_link_error";
    (void)events_.push(std::move(event));
}

As11BleRpcLink::Configuration As11BleRpcLink::configuration() const {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    const Configuration out = config_;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    return out;
}

bool As11BleRpcLink::reset_requested() const {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    const bool requested = reset_requested_;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    return requested;
}

void As11BleRpcLink::clear_reset_request() {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    reset_requested_ = false;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

void As11BleRpcLink::set_status(As11BleLinkState state,
                                const char *error) {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    status_.state = state;
    if (error) copy_text(status_.error, sizeof(status_.error), error);
    else if (state == As11BleLinkState::Ready ||
             state == As11BleLinkState::Disabled) {
        status_.error[0] = 0;
    }
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

void As11BleRpcLink::set_connected(bool connected, int rssi) {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    status_.connected = connected;
    status_.rssi = connected ? rssi : 0;
    if (connected) {
        copy_text(status_.address, sizeof(status_.address), config_.address);
    } else {
        status_.address[0] = 0;
    }
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

void As11BleRpcLink::set_authenticated(bool authenticated) {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    status_.authenticated = authenticated;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

}  // namespace aircannect
