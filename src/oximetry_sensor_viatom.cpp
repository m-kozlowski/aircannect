#include "ble_sensor_protocols.h"

#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "oximetry_codec.h"

namespace aircannect {

namespace {

static constexpr uint8_t VIATOM_CMD_CONFIG = 0x16;
static constexpr uint8_t VIATOM_CMD_READ_SENSORS = 0x17;
static constexpr uint8_t VIATOM_NO_PENDING_CMD = 0xff;
static constexpr uint32_t VIATOM_SENSOR_POLL_MS = 1000;
static constexpr uint32_t VIATOM_RESPONSE_TIMEOUT_MS = 1500;
static constexpr size_t VIATOM_WRITE_CHUNK_LEN = 20;
static constexpr uint32_t VIATOM_WRITE_CHUNK_DELAY_MS = 50;

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

bool decode_viatom_reading(const uint8_t *packet,
                           size_t len,
                           uint8_t &spo2,
                           uint16_t &pulse,
                           uint8_t &lead_state,
                           uint8_t &battery,
                           uint8_t &battery_state,
                           uint8_t &pi,
                           uint16_t &spo2_raw,
                           uint16_t &pulse_raw,
                           bool &invalid) {
    if (!packet || len < 9 || packet[0] != 0x55) return false;
    if (packet[1] != static_cast<uint8_t>(packet[2] ^ 0xff)) return false;

    const uint16_t payload_len =
        static_cast<uint16_t>(packet[5]) |
        (static_cast<uint16_t>(packet[6]) << 8);
    const size_t packet_len = static_cast<size_t>(payload_len) + 8;
    if (payload_len < 12 || len < packet_len) return false;
    if (crc8_ccitt(packet, packet_len - 1) != packet[packet_len - 1]) {
        return false;
    }

    const uint8_t *payload = packet + 7;
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

    Log::logf(CAT_OXI, LOG_DEBUG, "Sensor subscribed Viatom read\n");
    return true;
}

void BleSensorProtocolEngine::viatom_on_connected() {
    if (!viatom_.write) return;

    viatom_reset_rx();
    viatom_clear_pending();
    viatom_.last_poll_ms = 0;
    viatom_.need_time_sync = true;
}

void BleSensorProtocolEngine::viatom_reset() {
    viatom_.write = nullptr;
    viatom_reset_rx();
    viatom_clear_pending();
    viatom_.last_poll_ms = 0;
    viatom_.need_time_sync = false;
}

void BleSensorProtocolEngine::viatom_poll(uint32_t now_ms) {
    if (!client_ || !client_->isConnected() || !viatom_.write) return;

    if (viatom_.pending_cmd != VIATOM_NO_PENDING_CMD &&
        static_cast<int32_t>(now_ms - viatom_.pending_ms) >=
            static_cast<int32_t>(VIATOM_RESPONSE_TIMEOUT_MS)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom response timeout cmd=%s\n",
                  viatom_command_name(viatom_.pending_cmd));
        viatom_reset_rx();
        viatom_clear_pending();
    }
    if (viatom_.pending_cmd != VIATOM_NO_PENDING_CMD) return;

    if (viatom_.need_time_sync) {
        if (viatom_sync_datetime()) {
            viatom_.pending_cmd = VIATOM_CMD_CONFIG;
            viatom_.pending_ms = now_ms;
        }
        viatom_.need_time_sync = false;
        return;
    }

    if (static_cast<int32_t>(now_ms - viatom_.last_poll_ms) <
        static_cast<int32_t>(VIATOM_SENSOR_POLL_MS)) {
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
    (void)characteristic;
    (void)is_notify;
    BleSensorProtocolEngine *engine =
        active_.load(std::memory_order_acquire);
    if (engine) engine->viatom_notify(data, len);
}

void BleSensorProtocolEngine::viatom_notify(const uint8_t *data, size_t len) {
    if (!data || !len) return;

    const uint8_t *packet = data;
    size_t packet_len = len;
    if (data[0] == 0x55) {
        viatom_reset_rx();
        if (len >= 7) {
            const uint16_t payload_len =
                static_cast<uint16_t>(data[5]) |
                (static_cast<uint16_t>(data[6]) << 8);
            const size_t wanted = static_cast<size_t>(payload_len) + 8;
            if (wanted <= sizeof(viatom_.rx)) {
                viatom_.rx_want = wanted;
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "Sensor Viatom RX too large want=%u\n",
                          static_cast<unsigned>(wanted));
            }
        }
    }

    bool buffered = false;
    if (viatom_.rx_want) {
        if (viatom_.rx_len + len > sizeof(viatom_.rx)) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor Viatom RX overflow have=%u add=%u\n",
                      static_cast<unsigned>(viatom_.rx_len),
                      static_cast<unsigned>(len));
            viatom_reset_rx();
            return;
        }

        memcpy(viatom_.rx + viatom_.rx_len, data, len);
        viatom_.rx_len += len;
        if (viatom_.rx_len < viatom_.rx_want) return;

        packet = viatom_.rx;
        packet_len = viatom_.rx_len;
        buffered = true;
    }

    const uint8_t pending_command = viatom_.pending_cmd;
    viatom_clear_pending();
    if (pending_command != VIATOM_CMD_READ_SENSORS) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom RX ignored for cmd=%s\n",
                  viatom_command_name(pending_command));
        if (buffered) viatom_reset_rx();
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
        packet, packet_len, spo2, pulse, lead_state, battery,
        battery_state, pi, spo2_raw, pulse_raw, invalid);
    if (buffered) viatom_reset_rx();
    if (!decoded) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom RX read decode failed len=%u\n",
                  static_cast<unsigned>(packet_len));
        return;
    }

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Viatom reading %s spo2=%u pulse=%u lead=%u battery=%u battery_state=%u pi=%.1f\n",
              invalid ? "invalid" : "valid",
              static_cast<unsigned>(spo2),
              static_cast<unsigned>(pulse),
              static_cast<unsigned>(lead_state),
              static_cast<unsigned>(battery),
              static_cast<unsigned>(battery_state),
              static_cast<double>(pi) / 10.0);
    emit_sample(spo2_raw, pulse_raw, invalid, true, lead_state != 0);
}

void BleSensorProtocolEngine::viatom_reset_rx() {
    viatom_.rx_len = 0;
    viatom_.rx_want = 0;
}

void BleSensorProtocolEngine::viatom_clear_pending() {
    viatom_.pending_cmd = VIATOM_NO_PENDING_CMD;
    viatom_.pending_ms = 0;
}

bool BleSensorProtocolEngine::viatom_write_packet(
    uint8_t command,
    const uint8_t *payload,
    size_t payload_len) {
    if (!viatom_.write || payload_len > 0xffff) return false;

    const size_t packet_len = 7 + payload_len + 1;
    if (packet_len > 80) return false;

    uint8_t packet[80] = {};
    packet[0] = 0xaa;
    packet[1] = command;
    packet[2] = static_cast<uint8_t>(command ^ 0xff);
    packet[5] = static_cast<uint8_t>(payload_len & 0xff);
    packet[6] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    if (payload && payload_len) memcpy(packet + 7, payload, payload_len);
    packet[7 + payload_len] = crc8_ccitt(packet, 7 + payload_len);

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
