#include "as11_ble_rpc_link.h"

#include <ArduinoJson.h>
#include <ctype.h>
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
static constexpr TickType_t AS11_BLE_HANDOFF_WAIT = pdMS_TO_TICKS(5);
static constexpr uint32_t AS11_BLE_SCANNER_WAIT_MS =
    AC_OXIMETRY_SENSOR_SCAN_MS + 1000;

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
        case As11BleLinkState::Disconnecting: return "disconnecting";
        case As11BleLinkState::Quiesced: return "quiesced";
        case As11BleLinkState::Backoff: return "backoff";
    }
    return "unknown";
}

const char *as11_ble_pairing_state_name(As11BlePairingState state) {
    switch (state) {
        case As11BlePairingState::Idle: return "idle";
        case As11BlePairingState::Scanning: return "scanning";
        case As11BlePairingState::SelectDevice: return "select_device";
        case As11BlePairingState::Connecting: return "connecting";
        case As11BlePairingState::AwaitingPasskey:
            return "awaiting_passkey";
        case As11BlePairingState::Verifying: return "verifying";
        case As11BlePairingState::Saving: return "saving";
        case As11BlePairingState::Complete: return "complete";
        case As11BlePairingState::Failed: return "failed";
    }
    return "unknown";
}

bool As11BleRpcLink::begin() {
#if AC_BLE_ENABLED
    if (!requests_.begin() || !pairing_commands_.begin() ||
        !credential_updates_.begin() || !notifications_.begin() ||
        !events_.begin() || !fig_.begin()) {
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

void As11BleRpcLink::poll(uint32_t now_ms) {
    (void)now_ms;

    CredentialUpdate update;
    while (credential_updates_.pop(update)) {
        const bool stored = credential_store_ &&
            credential_store_(
                credential_store_context_,
                update.forget ? "" : update.address,
                update.forget ? "" : update.client_id,
                update.forget ? "" : update.master_key_hex);

        memset(&update, 0, sizeof(update));
        if (stored) {
            set_pairing_state(As11BlePairingState::Complete);
        } else {
            fail_pairing("credential_store_failed");
        }
    }
}

RpcLinkSendResult As11BleRpcLink::send(RpcPayloadView payload) {
    const As11BleLinkStatus snapshot = ble_status();
    if (!snapshot.enabled || !snapshot.authenticated ||
        controlled_disconnect_requested()) {
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

    if (!requests_.push(std::move(request), AS11_BLE_HANDOFF_WAIT)) {
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

void As11BleRpcLink::set_controlled_disconnect(bool requested) {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    if (controlled_disconnect_requested_ != requested) {
        controlled_disconnect_requested_ = requested;
        controlled_disconnect_complete_ = false;
    }
    portEXIT_CRITICAL(&mux_);
#else
    (void)requested;
#endif
}

bool As11BleRpcLink::controlled_disconnect_complete() const {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    const bool complete = !controlled_disconnect_requested_ ||
                          controlled_disconnect_complete_;
    portEXIT_CRITICAL(&mux_);
    return complete;
#else
    return true;
#endif
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
    out.ready = snapshot.authenticated &&
                !controlled_disconnect_requested();
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
    pairing_status_.paired = next.address[0] && next.client_id[0] &&
                             strlen(next.master_key_hex) ==
                                 AS11_BLE_KEY_HEX_BYTES;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

void As11BleRpcLink::set_credential_store(CredentialStore store,
                                           void *context) {
    credential_store_ = store;
    credential_store_context_ = context;
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

As11BlePairingStatus As11BleRpcLink::pairing_status() const {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    const As11BlePairingStatus out = pairing_status_;
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    return out;
}

bool As11BleRpcLink::request_pairing_scan() {
    PairingCommand command;
    command.kind = PairingCommandKind::Scan;
    return enqueue_pairing_command(command);
}

bool As11BleRpcLink::request_pairing_device(const char *address) {
    PairingCommand command;
    command.kind = PairingCommandKind::SelectDevice;
    if (!copy_text(command.value, sizeof(command.value), address)) {
        return false;
    }
    return enqueue_pairing_command(command);
}

bool As11BleRpcLink::submit_pairing_passkey(const char *passkey) {
    if (!as11_ble_pairing_passkey_valid(passkey)) return false;

    PairingCommand command;
    command.kind = PairingCommandKind::SubmitPasskey;
    if (!copy_text(command.value, sizeof(command.value), passkey)) {
        return false;
    }
    return enqueue_pairing_command(command);
}

bool As11BleRpcLink::cancel_pairing() {
    PairingCommand command;
    command.kind = PairingCommandKind::Cancel;
    return enqueue_pairing_command(command);
}

bool As11BleRpcLink::forget_pairing() {
    PairingCommand command;
    command.kind = PairingCommandKind::Forget;
    return enqueue_pairing_command(command);
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
    bool controlled_disconnect_started = false;

    while (true) {
        const Configuration config = configuration();
        const uint32_t now_ms = millis();

        if (controlled_disconnect_requested()) {
            if (!controlled_disconnect_started) {
                controlled_disconnect_started = true;
                set_authenticated(false);
                requests_.clear(pdMS_TO_TICKS(100));

                portENTER_CRITICAL(&mux_);
                queued_requests_ = 0;
                portEXIT_CRITICAL(&mux_);

                if (client_ && client_->isConnected()) {
                    set_status(As11BleLinkState::Disconnecting);
                    Log::logf(CAT_BLE, LOG_INFO,
                              "disconnecting AS11 before restart\n");
                    (void)client_->disconnect();
                }
            }

            if (client_ && client_->isConnected()) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            if (!controlled_disconnect_complete()) {
                disconnect_and_clear();
                ready_seen = false;
                set_status(As11BleLinkState::Quiesced);
                set_controlled_disconnect_complete(true);
                Log::logf(CAT_BLE, LOG_INFO,
                          "AS11 disconnected before restart\n");
            }

            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (controlled_disconnect_started) {
            controlled_disconnect_started = false;
            next_attempt_ms = 0;
            reconnect_delay_ms = AC_AS11_BLE_RECONNECT_MIN_MS;
        }

        PairingCommand pairing_command;
        if (pairing_commands_.pop(pairing_command)) {
            (void)handle_pairing_command(config, client_callbacks,
                                         pairing_command);
            memset(&pairing_command, 0, sizeof(pairing_command));
        }

        const As11BlePairingStatus pairing = pairing_status();
        if (pairing.state == As11BlePairingState::AwaitingPasskey &&
            static_cast<int32_t>(now_ms - pairing_passkey_deadline_ms_) >= 0) {
            fail_pairing("passkey_timeout");
            disconnect_and_clear();
        }
        if (pairing_active()) {
            drain_notifications(false);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!config.enabled) {
            if (client_) disconnect_and_clear();
            set_status(As11BleLinkState::Disabled);
            applied_generation = config.generation;
            ready_seen = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const bool configuration_changed =
            config.generation != applied_generation;
        if (configuration_changed || reset_requested()) {
            if (configuration_changed && ready_seen) {
                publish_disconnect("ble_configuration_changed");
            }

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
                publish_disconnect("ble_disconnected");
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
            Log::logf(CAT_BLE, LOG_INFO,
                      "AS11 session ready address=%s rssi=%d write=%s\n",
                      config.address, client_->getRssi(),
                      tx_write_without_response_ ? "command" : "request");
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
    tx_write_without_response_ = false;
    if (!client_ || client_->isConnected()) return;

    client_->deleteServices();
    if (NimBLEDevice::deleteClient(client_)) client_ = nullptr;
#endif
}

bool As11BleRpcLink::enqueue_pairing_command(PairingCommand command) {
    if (!begin()) return false;
    return pairing_commands_.push(std::move(command));
}

bool As11BleRpcLink::pairing_active() const {
    const As11BlePairingState state = pairing_status().state;
    return state == As11BlePairingState::Scanning ||
           state == As11BlePairingState::SelectDevice ||
           state == As11BlePairingState::Connecting ||
           state == As11BlePairingState::AwaitingPasskey ||
           state == As11BlePairingState::Verifying ||
           state == As11BlePairingState::Saving;
}

bool As11BleRpcLink::handle_pairing_command(
    const Configuration &config,
    As11BleClientCallbacks &callbacks,
    const PairingCommand &command) {
    if (command.kind == PairingCommandKind::Cancel) {
        disconnect_and_clear();
        clear_pairing_context();
        set_pairing_state(As11BlePairingState::Idle);
        return true;
    }

    if (command.kind == PairingCommandKind::Forget) {
        disconnect_and_clear();
        clear_pairing_context();
        set_pairing_state(As11BlePairingState::Saving);
        if (publish_forget()) return true;

        fail_pairing("credential_queue_full");
        return false;
    }

    if (!config.enabled) {
        fail_pairing("ble_transport_not_selected");
        return false;
    }

    if (command.kind == PairingCommandKind::Scan) {
        disconnect_and_clear();
        clear_pairing_context();
        set_pairing_state(As11BlePairingState::Scanning);
        if (!runtime_.ensure_started(config.runtime_name)) {
            fail_pairing("ble_init_failed");
            return false;
        }
        if (!scan_for_pairing_devices(config)) {
            if (pairing_status().state != As11BlePairingState::Failed) {
                fail_pairing("scan_failed");
            }
            return false;
        }

        const As11BlePairingStatus scanned = pairing_status();
        if (scanned.device_count == 0) {
            fail_pairing("device_not_found");
            return false;
        }

        set_pairing_state(As11BlePairingState::SelectDevice);
        return true;
    }

    if (command.kind == PairingCommandKind::SelectDevice) {
        if (pairing_status().state != As11BlePairingState::SelectDevice) {
            fail_pairing("device_selection_not_expected");
            return false;
        }
        return start_pairing_device(config, callbacks, command.value);
    }

    if (command.kind == PairingCommandKind::SubmitPasskey) {
        if (pairing_status().state !=
            As11BlePairingState::AwaitingPasskey) {
            fail_pairing("passkey_not_expected");
            return false;
        }
        return finish_pairing(command.value);
    }

    return false;
}

bool As11BleRpcLink::scan_for_pairing_devices(
    const Configuration &config) {
#if AC_BLE_ENABLED
    set_status(As11BleLinkState::WaitingForScanner);
    if (runtime_.scan_in_progress()) {
        Log::logf(CAT_BLE, LOG_INFO,
                  "AS11 pairing waiting for BLE scanner timeout_ms=%u\n",
                  static_cast<unsigned>(AS11_BLE_SCANNER_WAIT_MS));
    }

    BleRuntime::ScanLease scan_lease =
        runtime_.acquire_scan(pdMS_TO_TICKS(AS11_BLE_SCANNER_WAIT_MS));
    if (!scan_lease) {
        fail_pairing("scanner_busy");
        return false;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan) {
        fail_pairing("scanner_unavailable");
        return false;
    }

    portENTER_CRITICAL(&mux_);
    pairing_scan_active_ = true;
    pairing_status_.device_count = 0;
    memset(pairing_status_.devices, 0,
           sizeof(pairing_status_.devices));
    pairing_status_.selected_address[0] = 0;
    pairing_status_.selected_name[0] = 0;
    portEXIT_CRITICAL(&mux_);

    As11BleScanCallbacks callbacks(this);
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
    pairing_scan_active_ = false;
    const size_t device_count = pairing_status_.device_count;
    portEXIT_CRITICAL(&mux_);
    scan_lease.release();

    Log::logf(CAT_BLE, LOG_INFO,
              "AS11 pairing scan complete devices=%u\n",
              static_cast<unsigned>(device_count));

    (void)config;
    return true;
#else
    (void)config;
    return false;
#endif
}

bool As11BleRpcLink::start_pairing_device(
    const Configuration &config,
    As11BleClientCallbacks &callbacks,
    const char *address) {
    As11BlePairingDevice device;
    if (!find_pairing_device(address, device)) {
        fail_pairing("device_not_found");
        return false;
    }

    disconnect_and_clear();
    clear_pairing_context();
    set_pairing_selected(device);
    set_pairing_state(As11BlePairingState::Connecting);
    if (!runtime_.ensure_started(config.runtime_name) ||
        !connect_device(device, callbacks) || !discover_pipe()) {
        disconnect_and_clear();
        fail_pairing("connect_failed");
        return false;
    }

    char public_key[AS11_BLE_SRP_PUBLIC_HEX_LENGTH + 1] = {};
    PlainResponseField fields[] = {
        {"serverPk", pairing_server_key_, sizeof(pairing_server_key_), true},
        {"salt", pairing_salt_, sizeof(pairing_salt_), true},
    };
    if (!pairing_srp_.begin(public_key) ||
        !send_plain_request("StartKeyExchange", "clientPk", public_key, 1) ||
        !wait_plain_response(1, fields,
                             sizeof(fields) / sizeof(fields[0]))) {
        memset(public_key, 0, sizeof(public_key));
        disconnect_and_clear();
        fail_pairing("key_exchange_start_failed");
        return false;
    }
    memset(public_key, 0, sizeof(public_key));

    pairing_passkey_deadline_ms_ =
        millis() + AC_AS11_BLE_PAIRING_PASSKEY_TIMEOUT_MS;
    if (pairing_passkey_deadline_ms_ == 0) {
        pairing_passkey_deadline_ms_ = 1;
    }
    set_pairing_state(As11BlePairingState::AwaitingPasskey);
    return true;
}

bool As11BleRpcLink::finish_pairing(const char *passkey) {
    set_pairing_state(As11BlePairingState::Verifying);

    char client_proof[AS11_BLE_SRP_PROOF_HEX_LENGTH + 1] = {};
    char master_key[AS11_BLE_SRP_PROOF_HEX_LENGTH + 1] = {};
    char client_id[AC_AS11_BLE_CLIENT_ID_MAX + 1] = {};
    char server_proof[AS11_BLE_SRP_PROOF_HEX_LENGTH + 1] = {};
    char nonce[AS11_BLE_KEY_HEX_BYTES + 1] = {};
    PlainResponseField fields[] = {
        {"clientId", client_id, sizeof(client_id), true},
        {"serverConfirmation", server_proof, sizeof(server_proof), true},
        {"nonce", nonce, sizeof(nonce), true},
    };

    const bool exchanged = pairing_srp_.finish(
                               passkey, pairing_server_key_, pairing_salt_,
                               client_proof, master_key) &&
                           send_plain_request(
                               "ConfirmKeyExchange", "clientConfirmation",
                               client_proof, 2) &&
                           wait_plain_response(
                               2, fields,
                               sizeof(fields) / sizeof(fields[0])) &&
                           pairing_srp_.verify_server(server_proof) &&
                           crypto_.set_master_key_hex(master_key) &&
                           crypto_.derive_session_key(nonce);
    memset(client_proof, 0, sizeof(client_proof));
    memset(server_proof, 0, sizeof(server_proof));
    memset(nonce, 0, sizeof(nonce));
    if (!exchanged) {
        memset(master_key, 0, sizeof(master_key));
        memset(client_id, 0, sizeof(client_id));
        disconnect_and_clear();
        fail_pairing("key_exchange_failed");
        return false;
    }

    set_pairing_state(As11BlePairingState::Saving);
    const bool queued = publish_credentials(client_id, master_key);
    memset(master_key, 0, sizeof(master_key));
    memset(client_id, 0, sizeof(client_id));
    clear_pairing_context();
    if (queued) return true;

    disconnect_and_clear();
    fail_pairing("credential_queue_full");
    return false;
}

void As11BleRpcLink::fail_pairing(const char *error) {
    clear_pairing_context();
    set_pairing_state(As11BlePairingState::Failed, error);
}

void As11BleRpcLink::clear_pairing_context() {
    pairing_passkey_deadline_ms_ = 0;
    pairing_srp_.clear();
    memset(pairing_server_key_, 0, sizeof(pairing_server_key_));
    memset(pairing_salt_, 0, sizeof(pairing_salt_));
}

void As11BleRpcLink::set_pairing_state(As11BlePairingState state,
                                       const char *error) {
    As11BlePairingState previous = As11BlePairingState::Idle;
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    previous = pairing_status_.state;
    pairing_status_.state = state;
    pairing_status_.active =
        state == As11BlePairingState::Scanning ||
        state == As11BlePairingState::SelectDevice ||
        state == As11BlePairingState::Connecting ||
        state == As11BlePairingState::AwaitingPasskey ||
        state == As11BlePairingState::Verifying ||
        state == As11BlePairingState::Saving;
    pairing_status_.passkey_required =
        state == As11BlePairingState::AwaitingPasskey;
    if (error) {
        copy_text(pairing_status_.error, sizeof(pairing_status_.error),
                  error);
    } else if (state != As11BlePairingState::Failed) {
        pairing_status_.error[0] = 0;
    }
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif

    if (previous == state && !error) return;

    const log_level_t level =
        state == As11BlePairingState::Failed ? LOG_WARN : LOG_INFO;
    const char *state_name = as11_ble_pairing_state_name(state);
    if (error) {
        Log::logf(CAT_BLE, level,
                  "AS11 pairing state=%s error=%s\n",
                  state_name, error);
    } else {
        Log::logf(CAT_BLE, level,
                  "AS11 pairing state=%s\n", state_name);
    }
}

void As11BleRpcLink::set_pairing_selected(
    const As11BlePairingDevice &device) {
    pairing_device_ = device;
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    copy_text(pairing_status_.selected_address,
              sizeof(pairing_status_.selected_address), device.address);
    copy_text(pairing_status_.selected_name,
              sizeof(pairing_status_.selected_name), device.name);
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
}

bool As11BleRpcLink::find_pairing_device(
    const char *address,
    As11BlePairingDevice &device) const {
    if (!address || !address[0]) return false;

    bool found = false;
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
#endif
    for (size_t i = 0; i < pairing_status_.device_count; ++i) {
        if (strcasecmp(pairing_status_.devices[i].address, address) != 0) {
            continue;
        }
        device = pairing_status_.devices[i];
        found = true;
        break;
    }
#if AC_BLE_ENABLED
    portEXIT_CRITICAL(&mux_);
#endif
    return found;
}

bool As11BleRpcLink::publish_credentials(const char *client_id,
                                          const char *master_key_hex) {
    CredentialUpdate update;
    if (!copy_text(update.address, sizeof(update.address),
                   pairing_device_.address) ||
        !copy_text(update.client_id, sizeof(update.client_id), client_id) ||
        !copy_text(update.master_key_hex, sizeof(update.master_key_hex),
                   master_key_hex)) {
        memset(&update, 0, sizeof(update));
        return false;
    }

    const bool queued = credential_updates_.push(std::move(update));
    memset(&update, 0, sizeof(update));
    return queued;
}

bool As11BleRpcLink::publish_forget() {
    CredentialUpdate update;
    update.forget = true;
    return credential_updates_.push(std::move(update));
}

void As11BleRpcLink::note_scan_result(const NimBLEAdvertisedDevice *device) {
#if AC_BLE_ENABLED
    if (!device) return;

    const std::string address = device->getAddress().toString();
    const std::string name = device->getName();
    const bool matching_name = name.rfind("ResMed", 0) == 0;
    const bool matching_service = device->isAdvertisingService(
        NimBLEUUID(AS11_BLE_SERVICE_UUID));

    portENTER_CRITICAL(&mux_);
    if (pairing_scan_active_) {
        if (!matching_name || !matching_service) {
            portEXIT_CRITICAL(&mux_);
            return;
        }

        size_t index = pairing_status_.device_count;
        for (size_t i = 0; i < pairing_status_.device_count; ++i) {
            if (strcasecmp(pairing_status_.devices[i].address,
                           address.c_str()) == 0) {
                index = i;
                break;
            }
        }
        if (index >= AC_AS11_BLE_PAIRING_DEVICE_MAX) {
            size_t weakest = 0;
            for (size_t i = 1; i < AC_AS11_BLE_PAIRING_DEVICE_MAX; ++i) {
                if (pairing_status_.devices[i].rssi <
                    pairing_status_.devices[weakest].rssi) {
                    weakest = i;
                }
            }
            if (device->getRSSI() <=
                pairing_status_.devices[weakest].rssi) {
                portEXIT_CRITICAL(&mux_);
                return;
            }
            index = weakest;
        } else if (index == pairing_status_.device_count) {
            pairing_status_.device_count++;
        }

        As11BlePairingDevice &candidate = pairing_status_.devices[index];
        copy_text(candidate.address, sizeof(candidate.address),
                  address.c_str());
        copy_text(candidate.name, sizeof(candidate.name), name.c_str());
        candidate.address_type = device->getAddress().getType();
        candidate.rssi = device->getRSSI();
        portEXIT_CRITICAL(&mux_);
        return;
    }

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
    Log::logf(CAT_BLE, LOG_INFO,
              "AS11 disconnected reason=%d\n", reason);
#else
    (void)reason;
#endif
}

void As11BleRpcLink::note_notification(const uint8_t *data, size_t length) {
    RpcPayloadRef payload = copy_rpc_payload(data, length);
    if (payload && notifications_.push(std::move(payload),
                                       AS11_BLE_HANDOFF_WAIT)) {
        return;
    }

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

    As11BlePairingDevice device;
    copy_text(device.address, sizeof(device.address), address);
    device.address_type = address_type;
    device.rssi = rssi;
    return connect_device(device, client_callbacks);
#else
    (void)config;
    (void)client_callbacks;
    return false;
#endif
}

bool As11BleRpcLink::connect_device(
    const As11BlePairingDevice &device,
    As11BleClientCallbacks &client_callbacks) {
#if AC_BLE_ENABLED
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

    const NimBLEAddress peer(std::string(device.address),
                             device.address_type);
    if (!client_->connect(peer)) {
        set_status(As11BleLinkState::Backoff, "connect_failed");
        return false;
    }

    set_connected(true, device.rssi);
    return true;
#else
    (void)device;
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
    const bool tx_writable = tx_ &&
        (tx_->canWrite() || tx_->canWriteNoResponse());

    if (!tx_writable || !rx_ || !rx_->canNotify()) {
        set_status(As11BleLinkState::Backoff, "gatt_pipe_missing");
        return false;
    }
    tx_write_without_response_ = tx_->canWriteNoResponse();

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
    PlainResponseField session_fields[] = {
        {"challenge", challenge, sizeof(challenge), true},
        {"nonce", nonce, sizeof(nonce), true},
    };
    if (!send_plain_request("RequestSession", "clientId", config.client_id,
                            1) ||
        !wait_plain_response(
            1, session_fields,
            sizeof(session_fields) / sizeof(session_fields[0]))) {
        set_status(As11BleLinkState::Backoff, "session_request_failed");
        return false;
    }

    char response[AS11_BLE_KEY_HEX_BYTES + 1] = {};
    if (!crypto_.integrity_response_hex(challenge, response) ||
        !send_plain_request("CheckSessionIntegrity", "response", response,
                            2) ||
        !wait_plain_response(2, nullptr, 0) ||
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

    char json[768];
    const size_t length = serializeJson(document, json, sizeof(json));
    if (length == 0 || length >= sizeof(json)) return false;
    return write_fig(AS11_BLE_VCID_PLAINTEXT_REQUEST,
                     reinterpret_cast<const uint8_t *>(json), length);
}

bool As11BleRpcLink::wait_plain_response(
    uint32_t id,
    const PlainResponseField *fields,
    size_t field_count) {
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

            for (size_t i = 0; i < field_count; ++i) {
                const PlainResponseField &field = fields[i];
                if (!field.key || !field.value || field.value_size == 0) {
                    return false;
                }

                const char *value = document["result"][field.key] | "";
                if (!copy_text(field.value, field.value_size, value) ||
                    (field.required && !field.value[0])) {
                    return false;
                }
            }
            return true;
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
    const size_t chunk_count =
        (packet->size() + chunk_size - 1) / chunk_size;
    const bool request_response = !tx_write_without_response_;

    for (size_t offset = 0; offset < packet->size();
        offset += chunk_size) {
        const size_t remaining = packet->size() - offset;
        const size_t count = remaining < chunk_size ? remaining : chunk_size;

        if (!tx_->writeValue(packet->data() + offset, count,
                             request_response)) {
            Log::logf(
                CAT_BLE, LOG_WARN,
                "GATT write failed vcid=0x%04x chunk=%u/%u bytes=%u "
                "mtu=%u connected=%s\n",
                static_cast<unsigned>(vcid),
                static_cast<unsigned>(offset / chunk_size + 1),
                static_cast<unsigned>(chunk_count),
                static_cast<unsigned>(count),
                static_cast<unsigned>(client_->getMTU()),
                client_->isConnected() ? "yes" : "no");
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
                publish_error(as11_ble_fig_decode_state_name(state));
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
    if (!events_.push(std::move(event), AS11_BLE_HANDOFF_WAIT)) {
        publish_error("event_queue_full");
    }
}

void As11BleRpcLink::publish_disconnect(const char *detail) {
    RpcLinkEvent event;
    event.kind = RpcLinkEventKind::Disconnected;
    event.detail = detail ? detail : "ble_disconnected";
    if (!events_.push(std::move(event), AS11_BLE_HANDOFF_WAIT)) {
        Log::logf(CAT_BLE, LOG_WARN,
                  "link reset notification dropped\n");
    }
}

void As11BleRpcLink::publish_error(const char *detail) {
    const char *error = detail ? detail : "ble_link_error";
    Log::logf(CAT_BLE, LOG_WARN, "link error=%s\n", error);

    RpcLinkEvent event;
    event.kind = RpcLinkEventKind::FramingError;
    event.detail = error;
    (void)events_.push(std::move(event), AS11_BLE_HANDOFF_WAIT);
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

bool As11BleRpcLink::controlled_disconnect_requested() const {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    const bool requested = controlled_disconnect_requested_;
    portEXIT_CRITICAL(&mux_);
    return requested;
#else
    return false;
#endif
}

void As11BleRpcLink::set_controlled_disconnect_complete(bool complete) {
#if AC_BLE_ENABLED
    portENTER_CRITICAL(&mux_);
    controlled_disconnect_complete_ = complete;
    portEXIT_CRITICAL(&mux_);
#else
    (void)complete;
#endif
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
