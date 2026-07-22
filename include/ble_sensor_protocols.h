#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

class BleSensorProtocolEngine {
public:
    using SampleCallback = void (*)(void *context,
                                    uint16_t spo2_raw,
                                    uint16_t pulse_raw,
                                    bool invalid,
                                    bool contact_known,
                                    bool contact_present);

    void set_sample_callback(SampleCallback callback, void *context);

#if AC_OXIMETRY_BLE_ENABLED
    bool matches(const NimBLEAdvertisedDevice *device) const;
    bool subscribe(NimBLEClient *client);
#endif
    void on_connected();
    void reset();
    void poll(uint32_t now_ms);

private:
    enum class ActiveProtocol : uint8_t {
        None,
        Plx,
        Nonin,
        Viatom,
        Oxyii,
    };

    void emit_sample(uint16_t spo2_raw,
                     uint16_t pulse_raw,
                     bool invalid,
                     bool contact_known = false,
                     bool contact_present = false);

#if AC_OXIMETRY_BLE_ENABLED
    // PLX and Nonin
    bool subscribe_plx();
    bool subscribe_nonin();
    static void plx_notify(NimBLERemoteCharacteristic *characteristic,
                           uint8_t *data,
                           size_t len,
                           bool is_notify);
    static void nonin_notify(NimBLERemoteCharacteristic *characteristic,
                             uint8_t *data,
                             size_t len,
                             bool is_notify);

    // Viatom
    struct ViatomState {
        NimBLERemoteCharacteristic *write = nullptr;
        uint8_t rx[64] = {};
        size_t rx_len = 0;
        size_t rx_want = 0;
        uint8_t pending_cmd = 0xff;
        uint32_t pending_ms = 0;
        uint32_t last_poll_ms = 0;
        bool need_time_sync = false;
    };

    bool subscribe_viatom();
    void viatom_on_connected();
    void viatom_reset();
    void viatom_poll(uint32_t now_ms);
    void viatom_notify(const uint8_t *data, size_t len);
    void viatom_reset_rx();
    void viatom_clear_pending();
    bool viatom_write_packet(uint8_t command,
                             const uint8_t *payload,
                             size_t payload_len);
    bool viatom_sync_datetime();
    bool viatom_send_command(uint8_t command,
                             const uint8_t *payload,
                             size_t payload_len,
                             uint32_t now_ms);
    static void viatom_notify_callback(
        NimBLERemoteCharacteristic *characteristic,
        uint8_t *data,
        size_t len,
        bool is_notify);

    // OxyII
    static constexpr size_t OxyiiRxMax = 640;

    struct OxyiiState {
        NimBLERemoteCharacteristic *write = nullptr;
        uint8_t rx[OxyiiRxMax] = {};
        size_t rx_len = 0;
        size_t rx_want = 0;
        uint8_t pending_cmd = 0xfe;
        uint32_t pending_ms = 0;
        uint32_t last_poll_ms = 0;
        uint8_t sequence = 0;
        bool need_auth = false;
        bool need_setup = false;
        bool need_time_sync = false;
    };

    bool subscribe_oxyii();
    void oxyii_on_connected();
    void oxyii_reset();
    void oxyii_poll(uint32_t now_ms);
    void oxyii_notify(const uint8_t *data, size_t len);
    void oxyii_reset_rx();
    void oxyii_clear_pending();
    bool oxyii_write_frame(uint8_t command,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint8_t sequence);
    bool oxyii_send_command(uint8_t command,
                            const uint8_t *payload,
                            size_t payload_len,
                            uint32_t now_ms,
                            bool expect_reply = true);
    bool oxyii_send_auth(uint32_t now_ms);
    bool oxyii_sync_datetime(uint32_t now_ms);
    static void oxyii_notify_callback(
        NimBLERemoteCharacteristic *characteristic,
        uint8_t *data,
        size_t len,
        bool is_notify);

    static std::atomic<BleSensorProtocolEngine *> active_;
    NimBLEClient *client_ = nullptr;
    ViatomState viatom_;
    OxyiiState oxyii_;
#endif

    ActiveProtocol active_protocol_ = ActiveProtocol::None;
    SampleCallback sample_callback_ = nullptr;
    void *sample_context_ = nullptr;
};

}  // namespace aircannect
