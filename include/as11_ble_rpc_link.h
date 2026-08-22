#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

#include "as11_ble_crypto.h"
#include "as11_ble_fig.h"
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
    Backoff,
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

const char *as11_ble_link_state_name(As11BleLinkState state);

class As11BleRpcLink final : public RpcApplicationLink {
public:
    explicit As11BleRpcLink(BleRuntime &runtime) : runtime_(runtime) {}

    bool begin() override;
    void poll(uint32_t now_ms) override;
    RpcLinkSendResult send(RpcPayloadView payload) override;
    bool take_event(RpcLinkEvent &event) override;
    void reset() override;

    RpcApplicationLinkStatus status() const override;
    const char *name() const override { return "ble"; }

    void configure(bool enabled,
                   const char *runtime_name,
                   const char *address,
                   const char *client_id,
                   const char *master_key_hex);
    As11BleLinkStatus ble_status() const;

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

    // Worker lifecycle
    static void task_entry(void *context);
    void task_loop();
    void disconnect_and_clear();
    void release_client();

    // Discovery and GATT
    friend class As11BleClientCallbacks;
    friend class As11BleScanCallbacks;
    void note_scan_result(const NimBLEAdvertisedDevice *device);
    void note_disconnected(int reason);
    void note_notification(const uint8_t *data, size_t length);
    bool scan_and_connect(const Configuration &config,
                          As11BleClientCallbacks &callbacks);
    bool discover_pipe();

    // Session and application packets
    bool authenticate(const Configuration &config);
    bool send_plain_request(const char *method,
                            const char *key,
                            const char *value,
                            uint32_t id);
    bool wait_plain_response(uint32_t id,
                             char *challenge,
                             size_t challenge_size,
                             char *nonce,
                             size_t nonce_size);
    bool write_fig(uint16_t vcid, const uint8_t *data, size_t length);
    void drain_notifications(bool publish_application);
    void publish_packet(const As11BleFigPacket &packet);
    void publish_error(const char *detail);

    // Cross-task state
    Configuration configuration() const;
    bool reset_requested() const;
    void clear_reset_request();
    void set_status(As11BleLinkState state,
                    const char *error = nullptr);
    void set_connected(bool connected, int rssi = 0);
    void set_authenticated(bool authenticated);

    BleRuntime &runtime_;
    As11BleSessionCrypto crypto_;
    As11BleFigCodec fig_;
    MainLoopInbox<Request, AC_AS11_BLE_REQUEST_QUEUE_DEPTH> requests_;
    MainLoopInbox<RpcPayloadRef, AC_AS11_BLE_NOTIFICATION_QUEUE_DEPTH>
        notifications_;
    MainLoopInbox<RpcLinkEvent, AC_AS11_BLE_EVENT_QUEUE_DEPTH> events_;

#if AC_BLE_ENABLED
    NimBLEClient *client_ = nullptr;
    NimBLERemoteCharacteristic *tx_ = nullptr;
    NimBLERemoteCharacteristic *rx_ = nullptr;
    TaskHandle_t task_ = nullptr;
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif

    Configuration config_;
    As11BleLinkStatus status_;
    bool task_started_ = false;
    bool reset_requested_ = false;
    uint8_t queued_requests_ = 0;
    bool scan_match_found_ = false;
    uint8_t scan_match_type_ = 0;
    int scan_match_rssi_ = 0;
    char scan_match_address_[AC_AS11_BLE_ADDRESS_MAX + 1] = {};
};

}  // namespace aircannect
