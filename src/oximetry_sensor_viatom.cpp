#include "ble_sensor_protocols.h"

#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "oximetry_codec.h"
#include "o2ring_recording_import.h"

namespace aircannect {

namespace {

static constexpr uint8_t VIATOM_CMD_CONFIG = 0x16;
static constexpr uint8_t VIATOM_CMD_READ_SENSORS = 0x17;
static constexpr uint8_t VIATOM_NO_PENDING_CMD = 0xff;
static constexpr uint32_t VIATOM_SENSOR_POLL_MS = 1000;
static constexpr uint32_t VIATOM_RESPONSE_TIMEOUT_MS = 1500;
static constexpr size_t VIATOM_WRITE_CHUNK_LEN = 20;
static constexpr uint32_t VIATOM_WRITE_CHUNK_DELAY_MS = 50;
using FileCommand = O2RingRecordingImport::Command;

bool is_file_command(uint8_t cmd) {
    return cmd == uint8_t(FileCommand::Info) || cmd == uint8_t(FileCommand::Open) ||
           cmd == uint8_t(FileCommand::Close) || cmd == uint8_t(FileCommand::Read);
}

const char *viatom_command_name(uint8_t command) {
    switch (command) {
        case VIATOM_CMD_CONFIG:
            return "config";
        case VIATOM_CMD_READ_SENSORS:
            return "read_sensors";
        case VIATOM_NO_PENDING_CMD:
            return "none";
        default:
            return "unknown";
    }
}

bool decode_viatom_reading(const BleSensorFrameView &frame,
                           uint8_t &spo2,
                           uint16_t &pulse,
                           uint8_t &lead_state,
                           uint8_t &battery,
                           uint8_t &battery_state,
                           uint8_t &pi,
                           uint16_t &spo2_raw,
                           uint16_t &pulse_raw,
                           bool &invalid) {
    if (frame.payload_len < 12) return false;

    const uint8_t *payload = frame.payload;
    spo2 = payload[0];
    pulse = static_cast<uint16_t>(payload[1]) |
            (static_cast<uint16_t>(payload[2]) << 8);
    battery = payload[7];
    battery_state = payload[8];
    pi = payload[10];
    lead_state = payload[11] & 0x01;

    const bool valid = spo2 > 0 && spo2 != 0xff && spo2 <= 100 &&
                       pulse > 0 && pulse != 0xff && pulse < 250;
    spo2_raw = valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN;
    pulse_raw = valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN;
    invalid = !valid;
    return true;
}

}  // namespace

#if AC_OXIMETRY_BLE_ENABLED
bool BleSensorProtocolEngine::subscribe_viatom() {
    if (!client_) return false;

    NimBLERemoteService *service = client_->getService(NimBLEUUID(
        "14839AC4-7D7E-415C-9A42-167340CF2339"));
    if (!service) return false;

    Log::logf(CAT_OXI, LOG_DEBUG, "Sensor Viatom service found\n");
    NimBLERemoteCharacteristic *read =
        service->getCharacteristic(NimBLEUUID(
            "0734594A-A8E7-4B1A-A6B1-CD5243059A57"));
    if (!read || !read->canNotify() ||
        !read->subscribe(true, viatom_notify_callback)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom read subscribe failed\n");
        return false;
    }

    viatom_.write = service->getCharacteristic(NimBLEUUID(
        "8B00ACE7-EB0B-49B0-BBE9-9AEE0A26E1A3"));
    if (!viatom_.write) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Sensor Viatom write missing\n");
        return false;
    }

    notify_characteristic_.store(read, std::memory_order_release);
    Log::logf(CAT_OXI, LOG_DEBUG, "Sensor subscribed Viatom read\n");
    return true;
}

void BleSensorProtocolEngine::viatom_on_connected() {
    if (!viatom_.write) return;

    viatom_reset_rx();
    viatom_clear_pending();
    viatom_.last_poll_ms = 0;
    viatom_.initial_probe_complete = false;
    viatom_.need_time_sync = true;
}

void BleSensorProtocolEngine::viatom_reset() {
    viatom_.write = nullptr;
    viatom_reset_rx();
    viatom_clear_pending();
    viatom_.last_poll_ms = 0;
    viatom_.initial_probe_complete = false;
    viatom_.need_time_sync = false;
    viatom_.file_open = false;
    recordings_ = nullptr;
}

bool BleSensorProtocolEngine::viatom_poll_recordings(uint32_t now_ms) {
    // A cancelled import still releases the remote file on this connection.
    const bool importing = recordings_ && recordings_->active();
    FileCommand cmd = FileCommand::None;
    if (viatom_.file_open && (!importing || recordings_->awaiting_list())) {
        cmd = FileCommand::Close;
    } else if (importing) {
        cmd = recordings_->next(now_ms);
    }
    if (cmd == FileCommand::None) return false;

    const bool open = cmd == FileCommand::Open;
    const auto *payload = open ? reinterpret_cast<const uint8_t *>(recordings_->recording()) : nullptr;
    const uint16_t index = cmd == FileCommand::Read ? recordings_->block() : 0;
    if (!viatom_write_packet(uint8_t(cmd), payload, open ? 15 : 0, index)) {
        if (recordings_) recordings_->fail("ble_write_failed");
        client_->disconnect();
        return true;
    }
    viatom_.pending_cmd = uint8_t(cmd);
    viatom_.pending_ms = now_ms;
    return true;
}

void BleSensorProtocolEngine::viatom_poll(uint32_t now_ms) {
    if (!client_ || !client_->isConnected() || !viatom_.write) return;

    if (viatom_.pending_cmd != VIATOM_NO_PENDING_CMD &&
        static_cast<int32_t>(now_ms - viatom_.pending_ms) >=
            static_cast<int32_t>(VIATOM_RESPONSE_TIMEOUT_MS)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom response timeout cmd=%s\n",
                  viatom_command_name(viatom_.pending_cmd));
        if (is_file_command(viatom_.pending_cmd)) {
            // Legacy replies carry status, not an opcode. Do not let a late
            // file response be mistaken for a live reading on this link.
            if (recordings_) recordings_->fail("ble_response_timeout");
            client_->disconnect();
            return;
        }
        viatom_reset_rx();
        viatom_clear_pending();
    }
    if (viatom_.pending_cmd != VIATOM_NO_PENDING_CMD) return;

    if (!viatom_.initial_probe_complete) {
        if (static_cast<int32_t>(now_ms - viatom_.last_poll_ms) <
            static_cast<int32_t>(VIATOM_SENSOR_POLL_MS)) {
            return;
        }

        if (!viatom_send_command(
                VIATOM_CMD_READ_SENSORS, nullptr, 0, now_ms)) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor Viatom probe write failed\n");
        }
        viatom_.last_poll_ms = now_ms;
        return;
    }

    if (viatom_.need_time_sync && !(recordings_ && recordings_->awaiting_list())) {
        if (viatom_sync_datetime()) {
            viatom_.pending_cmd = VIATOM_CMD_CONFIG;
            viatom_.pending_ms = now_ms;
        }
        viatom_.need_time_sync = false;
        return;
    }

    if (static_cast<int32_t>(now_ms - viatom_.last_poll_ms) <
        static_cast<int32_t>(VIATOM_SENSOR_POLL_MS)) {
        if (now_ms - viatom_.last_poll_ms < VIATOM_SENSOR_POLL_MS / 2)
            viatom_poll_recordings(now_ms);
        return;
    }

    if (!viatom_send_command(VIATOM_CMD_READ_SENSORS, nullptr, 0, now_ms)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom poll write failed\n");
    }
    viatom_.last_poll_ms = now_ms;
}

void BleSensorProtocolEngine::viatom_notify_callback(
    NimBLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t len,
    bool is_notify) {
    (void)is_notify;
    BleSensorProtocolEngine *engine =
        active_.load(std::memory_order_acquire);
    if (engine) {
        (void)engine->enqueue_notification(
            ActiveProtocol::Viatom, characteristic, data, len);
    }
}

void BleSensorProtocolEngine::viatom_notify(const uint8_t *data, size_t len) {
    // The first seven bytes declare the frame length; a notification can
    // split that header too. Only file replies can outgrow the live buffer.
    if (is_file_command(viatom_.pending_cmd) && !viatom_.rx_want) {
        const size_t count = std::min(len, 7 - viatom_.rx_len);
        memcpy(viatom_.rx + viatom_.rx_len, data, count);
        viatom_.rx_len += count;
        data += count;
        len -= count;
        if (viatom_.rx_len < 7) return;
        if (!ble_sensor_frame_size(viatom_.rx, 7, 0x55, viatom_.rx_want)) {
            viatom_reset_rx();
            return;
        }
        if (viatom_.rx_want > sizeof(viatom_.rx)) {
            if (viatom_.pending_cmd != uint8_t(FileCommand::Info) &&
                viatom_.rx_want > O2RingRecordingImport::BlockSize + 8) {
                viatom_reset_rx();
                return;
            }
            viatom_.file_rx = LargeByteBuffer::allocate(viatom_.rx_want);
            if (!viatom_.file_rx) { viatom_reset_rx(); return; }
            memcpy(viatom_.file_rx->data(), viatom_.rx, 7);
        }
    }
    auto *rx = viatom_.file_rx ? viatom_.file_rx->data() : viatom_.rx;
    const size_t capacity = viatom_.file_rx ? viatom_.file_rx->size() : sizeof(viatom_.rx);
    const auto result = append_ble_sensor_frame(
        data, len, 0x55, rx, capacity,
        viatom_.rx_len, viatom_.rx_want);

    if (result == BleSensorFrameResult::Incomplete) return;
    if (result == BleSensorFrameResult::Invalid) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Sensor Viatom RX invalid fragment\n");
        viatom_reset_rx();
        return;
    }

    BleSensorFrameView frame;
    const bool valid_frame = viatom_.rx_len == viatom_.rx_want &&
        decode_ble_sensor_frame(rx, viatom_.rx_len, viatom_.rx_want, 0, frame);

    const auto owner = std::move(viatom_.file_rx);
    viatom_reset_rx();
    if (!valid_frame) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Sensor Viatom RX decode failed\n");
        return;
    }

    const uint8_t pending_command = viatom_.pending_cmd;
    if (is_file_command(pending_command)) {
        const auto cmd = static_cast<FileCommand>(pending_command);
        const uint16_t index = uint16_t(rx[3]) | uint16_t(rx[4]) << 8;
        viatom_clear_pending();
        if (cmd == FileCommand::Close) viatom_.file_open = false;
        if (frame.command != 0) {
            if (recordings_ && cmd == FileCommand::Open) recordings_->skip_open();
            else if (recordings_) recordings_->fail("sensor_rejected_file_command");
            return;
        }
        if (cmd == FileCommand::Open) viatom_.file_open = true;
        if (recordings_ && recordings_->active())
            recordings_->reply(cmd, index, frame.payload, frame.payload_len);
        return;
    }
    if (pending_command == VIATOM_CMD_CONFIG) {
        viatom_clear_pending();
        return;
    }
    if (pending_command != VIATOM_CMD_READ_SENSORS) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom RX ignored for cmd=%s\n",
                  viatom_command_name(pending_command));
        return;
    }

    uint16_t spo2_raw = PLX_SFLOAT_NAN;
    uint16_t pulse_raw = PLX_SFLOAT_NAN;
    uint8_t spo2 = 0;
    uint16_t pulse = 0;
    uint8_t lead_state = 0xff;
    uint8_t battery = 0xff;
    uint8_t battery_state = 0xff;
    uint8_t pi = 0;
    bool invalid = true;

    const bool decoded = decode_viatom_reading(
        frame, spo2, pulse, lead_state, battery,
        battery_state, pi, spo2_raw, pulse_raw, invalid);

    if (!decoded) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom RX read decode failed payload_len=%u\n",
                  static_cast<unsigned>(frame.payload_len));
        return;
    }

    viatom_clear_pending();
    const bool charging = battery_state == 1 || battery_state == 2;
    if (recordings_) recordings_->observe_contact(lead_state != 0, charging, millis());
    viatom_.initial_probe_complete = !charging || (recordings_ && recordings_->active());
    if (charging) viatom_.need_time_sync = false;

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Viatom reading %s spo2=%u pulse=%u lead=%u battery=%u battery_state=%u pi=%.1f\n",
              invalid ? "invalid" : "valid",
              static_cast<unsigned>(spo2),
              static_cast<unsigned>(pulse),
              static_cast<unsigned>(lead_state),
              static_cast<unsigned>(battery),
              static_cast<unsigned>(battery_state),
              static_cast<double>(pi) / 10.0);
    emit_sample(spo2_raw, pulse_raw, invalid, true, lead_state != 0,
                charging);
}

void BleSensorProtocolEngine::viatom_reset_rx() {
    viatom_.rx_len = 0;
    viatom_.rx_want = 0;
    viatom_.file_rx.reset();
}

void BleSensorProtocolEngine::viatom_clear_pending() {
    viatom_.pending_cmd = VIATOM_NO_PENDING_CMD;
    viatom_.pending_ms = 0;
}

bool BleSensorProtocolEngine::viatom_write_packet(
    uint8_t command,
    const uint8_t *payload,
    size_t payload_len, uint16_t index) {
    if (!viatom_.write) return false;

    uint8_t packet[80] = {};
    if (!write_ble_sensor_frame_header(packet, sizeof(packet), 0xaa,
                                       command, payload_len)) {
        return false;
    }
    const size_t packet_len = payload_len + 8;
    packet[3] = static_cast<uint8_t>(index);
    packet[4] = static_cast<uint8_t>(index >> 8);
    if (payload && payload_len) memcpy(packet + 7, payload, payload_len);
    write_ble_sensor_frame_crc(packet, packet_len);

    for (size_t offset = 0; offset < packet_len;
         offset += VIATOM_WRITE_CHUNK_LEN) {
        size_t chunk_len = packet_len - offset;
        if (chunk_len > VIATOM_WRITE_CHUNK_LEN) {
            chunk_len = VIATOM_WRITE_CHUNK_LEN;
        }
        if (!viatom_.write->writeValue(packet + offset, chunk_len, false)) {
            return false;
        }
        if (offset + chunk_len < packet_len) {
            vTaskDelay(pdMS_TO_TICKS(VIATOM_WRITE_CHUNK_DELAY_MS));
        }
    }
    return true;
}

bool BleSensorProtocolEngine::viatom_sync_datetime() {
    if (!viatom_.write) return false;

    const time_t now = time(nullptr);
    if (now < 1704067200) return false;

    struct tm local = {};
    localtime_r(&now, &local);

    char payload[48] = {};
    if (!strftime(payload, sizeof(payload),
                  "{\"SetTIME\":\"%Y-%m-%d,%H:%M:%S\"}", &local)) {
        return false;
    }

    const bool written = viatom_write_packet(
        VIATOM_CMD_CONFIG,
        reinterpret_cast<const uint8_t *>(payload), strlen(payload));
    Log::logf(CAT_OXI, written ? LOG_INFO : LOG_WARN,
              written ? "Sensor Viatom datetime set: %s\n"
                      : "Sensor Viatom datetime write failed: %s\n",
              payload);
    return written;
}

bool BleSensorProtocolEngine::viatom_send_command(
    uint8_t command,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms) {
    if (!viatom_write_packet(command, payload, payload_len)) return false;

    viatom_.pending_cmd = command;
    viatom_.pending_ms = now_ms;
    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Viatom TX cmd=%s payload_len=%u\n",
              viatom_command_name(command),
              static_cast<unsigned>(payload_len));
    return true;
}
#endif

}  // namespace aircannect
