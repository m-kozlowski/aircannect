#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "ble_sensor_frame.h"
#include "large_byte_buffer.h"

#if AC_OXIMETRY_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

namespace aircannect {

class O2RingRecordingImport;

class BleSensorProtocolEngine {
public:
    using SampleCallback = void (*)(void *context,
                                    uint16_t spo2_raw,
                                    uint16_t pulse_raw,
                                    bool invalid,
                                    bool contact_known,
                                    bool contact_present,
                                    bool charging, float pi_percent, int16_t motion,
                                    uint32_t observed_ms);

    bool begin();
    void set_sample_callback(SampleCallback callback, void *context);

#if AC_OXIMETRY_BLE_ENABLED
    uint32_t notification_drops() const { return notification_drops_.load(); }
    bool matches(const NimBLEAdvertisedDevice *device) const;
    bool subscribe(NimBLEClient *client, const char *name);
#endif
    void on_connected();
    void invalidate_connection();
    void reset();
    void poll(uint32_t now_ms, O2RingRecordingImport *recordings = nullptr,
              bool history_ring = false);
    bool uses_recording_protocol(bool oxyii) const {
        return active_protocol_ == (oxyii ? ActiveProtocol::Oxyii : ActiveProtocol::Viatom);
    }

private:
    enum class ActiveProtocol : uint8_t {
        None,
        Plx,
        Nonin,
        Viatom,
        Oxyii,
        Pc60fw,
    };

    void emit_sample(uint16_t spo2_raw,
                     uint16_t pulse_raw,
                     bool invalid,
                     bool contact_known = false,
                     bool contact_present = false,
                     bool charging = false, float pi_percent = -1, int16_t motion = -1);

#if AC_OXIMETRY_BLE_ENABLED
    bool observe_wear(bool evidence, bool charging, uint32_t now_ms);
    bool worn_ = false;
    uint32_t last_worn_ms_ = 0;

    // NimBLE host callback -> sensor worker SPSC mailbox.
    static constexpr size_t NotificationQueueCapacity = 8;
    static constexpr size_t NotificationMaxBytes = BLE_ATT_MTU_MAX - 3;

    struct NotificationSlot {
        uint32_t generation = 0;
        uint32_t preceding_drops = 0;
        uint32_t observed_ms = 0;
        ActiveProtocol protocol = ActiveProtocol::None;
        NimBLERemoteCharacteristic *characteristic = nullptr;
        uint16_t length = 0;
        uint8_t bytes[NotificationMaxBytes] = {};
    };

    bool enqueue_notification(ActiveProtocol protocol,
                              NimBLERemoteCharacteristic *characteristic,
                              const uint8_t *data,
                              size_t len);
    void drain_notifications();
    void process_notification(const NotificationSlot &notification);
    void reset_fragment_assemblers();

    // PLX and Nonin
    bool plx_spot_check_ = false;
    bool subscribe_plx();
    bool subscribe_nonin();
    void process_plx_notification(const uint8_t *data, size_t len);
    void process_nonin_notification(const uint8_t *data, size_t len);
    static void plx_notify(NimBLERemoteCharacteristic *characteristic,
                           uint8_t *data,
                           size_t len,
                           bool is_notify);
    static void nonin_notify(NimBLERemoteCharacteristic *characteristic,
                             uint8_t *data,
                             size_t len,
                             bool is_notify);

    // PC-60FW push stream: four-byte header and one-byte length.
    static bool pc60fw_name(const char *name);
    bool subscribe_pc60fw();
    void pc60fw_notify(const uint8_t *data, size_t len);
    uint8_t pc60fw_rx_[4 + UINT8_MAX];
    size_t pc60fw_used_ = 0;

    // Viatom
    enum class ClockStep : uint8_t { Probe, AwaitInfo, Save, Set, Done };
    struct ViatomState {
        NimBLERemoteCharacteristic *write = nullptr;
        uint8_t rx[64] = {};
        size_t rx_len = 0;
        size_t rx_want = 0;
        uint8_t pending_cmd = 0xff;
        uint32_t pending_ms = 0;
        uint32_t last_poll_ms = 0;
        bool initial_probe_complete = false;
        ClockStep clock_step = ClockStep::Done;
        bool file_open = false;
        std::unique_ptr<LargeByteBuffer> file_rx;
    };

    bool subscribe_viatom();
    void viatom_on_connected();
    void viatom_reset();
    void viatom_poll(uint32_t now_ms);
    void viatom_notify(const uint8_t *data, size_t len);
    void viatom_reset_rx();
    void viatom_clear_pending();
    bool viatom_poll_recordings(uint32_t now_ms);
    bool viatom_write_packet(uint8_t command,
                             const uint8_t *payload,
                             size_t payload_len, uint16_t index = 0);
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
    enum class OxyiiFileStep { Idle, CloseForList, List, Footer, Release, SaveClock };

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
        bool need_identity = false, off_finger = false, file_open = false;
        OxyiiFileStep file_step = OxyiiFileStep::Idle;
        uint32_t file_size = 0, read_offset = 0, info_rtt_ms = 0;
        int64_t wall_ms = 0, info_utc_ms = 0;
        std::unique_ptr<LargeByteBuffer> list_rx;
    };

    bool subscribe_oxyii();
    void oxyii_on_connected();
    void oxyii_reset();
    void oxyii_poll(uint32_t now_ms);
    void oxyii_poll_history(uint32_t now_ms);
    void oxyii_history_reply(uint8_t command, uint8_t *payload, size_t length, uint32_t rtt);
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
    std::atomic<NimBLERemoteCharacteristic *> notify_characteristic_{nullptr};
    std::atomic<uint32_t> connection_generation_{1};
    std::atomic<uint32_t> notification_read_sequence_{0};
    std::atomic<uint32_t> notification_write_sequence_{0};
    uint32_t consumed_drops_ = 0;
    std::atomic<uint32_t> notification_drops_{0};
    NotificationSlot *notification_queue_ = nullptr;
    std::atomic<TaskHandle_t> notification_worker_{nullptr};
    uint32_t reported_notification_drops_ = 0;
    NimBLEClient *client_ = nullptr;
    ViatomState viatom_;
    O2RingRecordingImport *recordings_ = nullptr;
    bool history_ring_ = false;
    OxyiiState oxyii_;
#endif

    ActiveProtocol active_protocol_ = ActiveProtocol::None;
    SampleCallback sample_callback_ = nullptr;
    uint32_t observed_ms_ = 0;
    void *sample_context_ = nullptr;
};

}  // namespace aircannect
