#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

#include "as11_ble_crypto.h"
#include "as11_ble_fig.h"
#include "as11_ble_srp.h"
#include "ble_runtime.h"
#include "board.h"
#include "main_loop_inbox.h"
#include "rpc_application_link.h"

#if AC_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

class NimBLEAdvertisedDevice;

namespace aircannect {

class As11BleClientCallbacks;
class As11BleScanCallbacks;

enum class As11BleLinkState : uint8_t {
    Disabled,
    MissingCredentials,
    WaitingForScanner,
    Scanning,
    Connecting,
    Discovering,
    Authenticating,
    Ready,
    Disconnecting,
    Quiesced,
    Backoff,
};

enum class As11BlePairingState : uint8_t {
    Idle,
    Scanning,
    SelectDevice,
    Connecting,
    AwaitingPasskey,
    Verifying,
    Saving,
    Complete,
    Failed,
};

struct As11BleLinkStatus {
    As11BleLinkState state = As11BleLinkState::Disabled;
    bool enabled = false;
    bool connected = false;
    bool authenticated = false;
    int rssi = 0;
    uint32_t reconnects = 0;
    uint32_t notification_drops = 0;
    uint32_t framing_errors = 0;
    char address[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
    char error[64] = {};
};

struct As11BlePairingDevice {
    char address[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
    char name[AC_AS11_BLE_PAIRING_DEVICE_NAME_MAX + 1] = {};
    uint8_t address_type = 0;
    int rssi = 0;
};

struct As11BlePairingStatus {
    uint32_t revision = 1;
    As11BlePairingState state = As11BlePairingState::Idle;
    bool paired = false;
    bool active = false;
    bool passkey_required = false;
    uint8_t device_count = 0;
    As11BlePairingDevice devices[AC_AS11_BLE_PAIRING_DEVICE_MAX];
    char selected_address[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
    char selected_name[AC_AS11_BLE_PAIRING_DEVICE_NAME_MAX + 1] = {};
    char error[64] = {};
};

const char *as11_ble_link_state_name(As11BleLinkState state);
const char *as11_ble_pairing_state_name(As11BlePairingState state);

class As11BleRpcLink final : public RpcApplicationLink {
public:
    using CredentialStore = bool (*)(void *context,
                                     const char *address,
                                     const char *client_id,
                                     const char *master_key_hex);

    explicit As11BleRpcLink(BleRuntime &runtime) : runtime_(runtime) {}

    // RpcApplicationLink
    bool begin() override;
    void poll(uint32_t now_ms) override;
    RpcLinkSendResult send(RpcPayloadView payload) override;
    bool take_event(RpcLinkEvent &event) override;
    void reset() override;
    void set_controlled_disconnect(bool requested) override;
    bool controlled_disconnect_complete() const override;
    RpcApplicationLinkStatus status() const override;
    const char *name() const override { return "ble"; }

    // Configuration and status
    void configure(bool enabled,
                   const char *runtime_name,
                   const char *address,
                   const char *client_id,
                   const char *master_key_hex);
    void set_credential_store(CredentialStore store, void *context);
    As11BleLinkStatus ble_status() const;
    uint32_t pairing_revision() const;
    As11BlePairingStatus pairing_status() const;
    bool request_reconnect();

    // First pairing
    bool request_pairing_scan();
    bool request_pairing_device(const char *address);
    bool submit_pairing_passkey(const char *passkey);
    bool cancel_pairing();
    bool forget_pairing();

private:
    struct Request {
        RpcPayloadRef payload;
    };

    struct Configuration {
        bool enabled = false;
        uint32_t generation = 0;
        char runtime_name[AC_BLE_DEVICE_NAME_MAX + 1] = {};
        char address[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
        char client_id[AC_AS11_BLE_CLIENT_ID_MAX + 1] = {};
        char master_key_hex[AS11_BLE_KEY_HEX_BYTES + 1] = {};
    };

    enum class PairingCommandKind : uint8_t {
        Scan,
        SelectDevice,
        SubmitPasskey,
        Cancel,
        Forget,
    };

    struct PairingCommand {
        PairingCommandKind kind = PairingCommandKind::Scan;
        char value[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
    };

    struct CredentialUpdate {
        bool forget = false;
        char address[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
        char client_id[AC_AS11_BLE_CLIENT_ID_MAX + 1] = {};
        char master_key_hex[AS11_BLE_KEY_HEX_BYTES + 1] = {};
    };

    struct PlainResponseField {
        const char *key = nullptr;
        char *value = nullptr;
        size_t value_size = 0;
        bool required = true;
    };

    // Worker lifecycle
    static void task_entry(void *context);
    void task_loop();
    void disconnect_and_clear();
    void release_client();

    // First pairing
    bool enqueue_pairing_command(PairingCommand command);
    bool pairing_active() const;
    bool handle_pairing_command(const Configuration &config,
                                As11BleClientCallbacks &callbacks,
                                const PairingCommand &command);
    bool scan_for_pairing_devices(const Configuration &config);
    bool start_pairing_device(const Configuration &config,
                              As11BleClientCallbacks &callbacks,
                              const char *address);
    bool finish_pairing(const char *passkey);
    void fail_pairing(const char *error);
    void clear_pairing_context();
    void bump_pairing_revision_locked();
    void set_pairing_state(As11BlePairingState state,
                           const char *error = nullptr);
    void set_pairing_selected(const As11BlePairingDevice &device);
    bool find_pairing_device(const char *address,
                             As11BlePairingDevice &device) const;
    bool publish_credentials(const char *client_id,
                             const char *master_key_hex);
    bool publish_forget();

    // Discovery and GATT
    friend class As11BleClientCallbacks;
    friend class As11BleScanCallbacks;
    void note_scan_result(const NimBLEAdvertisedDevice *device);
    void note_disconnected(int reason);
    void note_notification(const uint8_t *data, size_t length);
    bool scan_and_connect(const Configuration &config,
                          As11BleClientCallbacks &callbacks);
    bool connect_device(const As11BlePairingDevice &device,
                        As11BleClientCallbacks &callbacks);
    bool discover_pipe();

    // Session and application packets
    bool authenticate(const Configuration &config);
    bool send_plain_request(const char *method,
                            const char *key,
                            const char *value,
                            uint32_t id);
    bool wait_plain_response(uint32_t id,
                             const PlainResponseField *fields,
                             size_t field_count);
    bool write_fig(uint16_t vcid, const uint8_t *data, size_t length);
    void drain_notifications(bool publish_application);
    void publish_packet(const As11BleFigPacket &packet);
    void publish_disconnect(const char *detail);
    void publish_error(const char *detail);

    // Cross-task state
    Configuration configuration() const;
    bool reset_requested() const;
    void clear_reset_request();
    bool take_reconnect_request();
    bool controlled_disconnect_requested() const;
    void set_controlled_disconnect_complete(bool complete);
    void set_status(As11BleLinkState state,
                    const char *error = nullptr);
    void set_connected(bool connected, int rssi = 0);
    void set_authenticated(bool authenticated);

    BleRuntime &runtime_;
    As11BleSessionCrypto crypto_;
    As11BleFigCodec fig_;
    As11BleSrpClient pairing_srp_;

    MainLoopInbox<Request, AC_AS11_BLE_REQUEST_QUEUE_DEPTH> requests_;
    MainLoopInbox<PairingCommand,
                  AC_AS11_BLE_PAIRING_COMMAND_QUEUE_DEPTH> pairing_commands_;
    MainLoopInbox<CredentialUpdate,
                  AC_AS11_BLE_CREDENTIAL_QUEUE_DEPTH> credential_updates_;
    MainLoopInbox<RpcPayloadRef, AC_AS11_BLE_NOTIFICATION_QUEUE_DEPTH>
        notifications_;
    MainLoopInbox<RpcLinkEvent, AC_AS11_BLE_EVENT_QUEUE_DEPTH> events_;

#if AC_BLE_ENABLED
    NimBLEClient *client_ = nullptr;
    NimBLERemoteCharacteristic *tx_ = nullptr;
    NimBLERemoteCharacteristic *rx_ = nullptr;
    bool tx_write_without_response_ = false;
    TaskHandle_t task_ = nullptr;
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif

    // Configuration and public state
    Configuration config_;
    As11BleLinkStatus status_;
    As11BlePairingStatus pairing_status_;
    CredentialStore credential_store_ = nullptr;
    void *credential_store_context_ = nullptr;
    bool task_started_ = false;
    bool reset_requested_ = false;
    bool reconnect_requested_ = false;
    bool controlled_disconnect_requested_ = false;
    bool controlled_disconnect_complete_ = false;
    uint8_t queued_requests_ = 0;

    // Scan state
    bool scan_match_found_ = false;
    uint8_t scan_match_type_ = 0;
    int scan_match_rssi_ = 0;
    char scan_match_address_[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
    bool pairing_scan_active_ = false;

    // Pairing exchange state
    uint32_t pairing_passkey_deadline_ms_ = 0;
    As11BlePairingDevice pairing_device_;
    char pairing_server_key_[AS11_BLE_SRP_PUBLIC_HEX_LENGTH + 1] = {};
    char pairing_salt_[129] = {};
};

}  // namespace aircannect
