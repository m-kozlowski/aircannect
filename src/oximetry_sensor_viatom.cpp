#include "oximetry_sensor_protocols.h"

#include <string.h>
#include <time.h>

#include "debug_log.h"

namespace aircannect {

namespace {

static constexpr uint8_t VIATOM_CMD_CONFIG = 0x16;
static constexpr uint8_t VIATOM_CMD_READ_SENSORS = 0x17;
static constexpr uint32_t VIATOM_SENSOR_POLL_MS = 1000;
static constexpr uint32_t VIATOM_RESPONSE_TIMEOUT_MS = 1500;
static constexpr size_t VIATOM_WRITE_CHUNK_LEN = 20;
static constexpr uint32_t VIATOM_WRITE_CHUNK_DELAY_MS = 50;
static constexpr uint8_t VIATOM_NO_PENDING_CMD = 0xff;

#if AC_OXIMETRY_BLE_ENABLED
NimBLERemoteCharacteristic *sensor_viatom_write = nullptr;
uint8_t sensor_viatom_rx[64] = {};
size_t sensor_viatom_rx_len = 0;
size_t sensor_viatom_rx_want = 0;
uint8_t sensor_viatom_pending_cmd = VIATOM_NO_PENDING_CMD;
uint32_t sensor_viatom_pending_ms = 0;
uint32_t sensor_viatom_last_poll_ms = 0;
bool sensor_viatom_need_time_sync = false;
#endif

void viatom_reset_rx() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_viatom_rx_len = 0;
    sensor_viatom_rx_want = 0;
#endif
}

void viatom_clear_pending() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_viatom_pending_cmd = VIATOM_NO_PENDING_CMD;
    sensor_viatom_pending_ms = 0;
#endif
}

const char *viatom_cmd_name(uint8_t cmd) {
    switch (cmd) {
        case VIATOM_CMD_CONFIG: return "config";
        case VIATOM_CMD_READ_SENSORS: return "read_sensors";
        case VIATOM_NO_PENDING_CMD: return "none";
        default: return "unknown";
    }
}

bool viatom_decode_reading(const uint8_t *packet,
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
    const bool valid =
        spo2 > 0 && spo2 != 0xff && spo2 <= 100 &&
        pulse > 0 && pulse != 0xff && pulse < 250;
    spo2_raw = valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN;
    pulse_raw = valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN;
    invalid = !valid;
    return true;
}

bool viatom_write_packet(NimBLERemoteCharacteristic *write_chr,
                         uint8_t cmd,
                         const uint8_t *payload,
                         size_t payload_len) {
    if (!write_chr || payload_len > 0xffff) return false;
    const size_t packet_len = 7 + payload_len + 1;
    if (packet_len > 80) return false;

    uint8_t packet[80] = {};
    packet[0] = 0xaa;
    packet[1] = cmd;
    packet[2] = static_cast<uint8_t>(cmd ^ 0xff);
    packet[3] = 0x00;
    packet[4] = 0x00;
    packet[5] = static_cast<uint8_t>(payload_len & 0xff);
    packet[6] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    if (payload && payload_len) {
        memcpy(packet + 7, payload, payload_len);
    }
    packet[7 + payload_len] = crc8_ccitt(packet, 7 + payload_len);

    for (size_t offset = 0; offset < packet_len;
         offset += VIATOM_WRITE_CHUNK_LEN) {
        size_t chunk_len = packet_len - offset;
        if (chunk_len > VIATOM_WRITE_CHUNK_LEN) {
            chunk_len = VIATOM_WRITE_CHUNK_LEN;
        }
        if (!write_chr->writeValue(packet + offset, chunk_len, false)) {
            return false;
        }
        if (offset + chunk_len < packet_len) {
            vTaskDelay(pdMS_TO_TICKS(VIATOM_WRITE_CHUNK_DELAY_MS));
        }
    }
    return true;
}

bool viatom_sync_datetime(NimBLERemoteCharacteristic *write_chr) {
    if (!write_chr) return false;

    const time_t now = time(nullptr);
    if (now < 1704067200) return false;  // 2024-01-01T00:00:00Z

    struct tm local = {};
    localtime_r(&now, &local);

    char payload[48] = {};
    if (!strftime(payload, sizeof(payload),
                  "{\"SetTIME\":\"%Y-%m-%d,%H:%M:%S\"}", &local)) {
        return false;
    }

    const bool ok = viatom_write_packet(
        write_chr,
        VIATOM_CMD_CONFIG,
        reinterpret_cast<const uint8_t *>(payload),
        strlen(payload));
    if (ok) {
        Log::logf(CAT_OXI, LOG_INFO,
                  "Sensor Viatom datetime set: %s\n",
                  payload);
    } else {
        Log::logf(CAT_OXI, LOG_WARN,
                  "Sensor Viatom datetime write failed\n");
    }
    return ok;
}

bool viatom_send_command(NimBLERemoteCharacteristic *write_chr,
                         uint8_t cmd,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint32_t now_ms) {
    if (!viatom_write_packet(write_chr, cmd, payload, payload_len)) {
        return false;
    }
    sensor_viatom_pending_cmd = cmd;
    sensor_viatom_pending_ms = now_ms;
    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Viatom TX cmd=%s payload_len=%u\n",
              viatom_cmd_name(cmd),
              static_cast<unsigned>(payload_len));
    return true;
}

void sensor_viatom_notify_cb(NimBLERemoteCharacteristic *chr,
                             uint8_t *data,
                             size_t len,
                             bool is_notify) {
    (void)chr;
    (void)is_notify;
    if (!data || !len) return;

    const uint8_t *packet = data;
    size_t packet_len = len;

    if (data[0] == 0x55) {
        sensor_viatom_rx_len = 0;
        sensor_viatom_rx_want = 0;
        if (len >= 7) {
            const uint16_t payload_len =
                static_cast<uint16_t>(data[5]) |
                (static_cast<uint16_t>(data[6]) << 8);
            const size_t want = static_cast<size_t>(payload_len) + 8;
            if (want <= sizeof(sensor_viatom_rx)) {
                sensor_viatom_rx_want = want;
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "Sensor Viatom RX too large want=%u\n",
                          static_cast<unsigned>(want));
            }
        }
    }

    bool buffered_packet = false;
    bool buffered_packet_complete = false;
    if (sensor_viatom_rx_want) {
        if (sensor_viatom_rx_len + len > sizeof(sensor_viatom_rx)) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor Viatom RX overflow have=%u add=%u\n",
                      static_cast<unsigned>(sensor_viatom_rx_len),
                      static_cast<unsigned>(len));
            viatom_reset_rx();
            return;
        }
        memcpy(sensor_viatom_rx + sensor_viatom_rx_len, data, len);
        sensor_viatom_rx_len += len;
        if (sensor_viatom_rx_len < sensor_viatom_rx_want) {
            return;
        }
        packet = sensor_viatom_rx;
        packet_len = sensor_viatom_rx_len;
        buffered_packet = true;
        buffered_packet_complete =
            sensor_viatom_rx_len >= sensor_viatom_rx_want;
    }

    const uint8_t pending_cmd = sensor_viatom_pending_cmd;
    viatom_clear_pending();
    if (pending_cmd != VIATOM_CMD_READ_SENSORS) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom RX ignored for cmd=%s\n",
                  viatom_cmd_name(pending_cmd));
        if (buffered_packet_complete) viatom_reset_rx();
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
    if (!viatom_decode_reading(packet, packet_len, spo2, pulse,
                               lead_state, battery, battery_state, pi,
                               spo2_raw, pulse_raw,
                               invalid)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom RX read decode failed len=%u\n",
                  static_cast<unsigned>(packet_len));
        if (buffered_packet_complete) viatom_reset_rx();
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

    if (buffered_packet) {
        viatom_reset_rx();
    }

    if (sensor_owner) {
        sensor_owner->on_sensor_sample(
            spo2_raw, pulse_raw, invalid, true, lead_state != 0);
    }
}

}  // namespace

#if AC_OXIMETRY_BLE_ENABLED
bool sensor_subscribe_viatom(NimBLEClient *client) {
    if (!client) return false;
    NimBLERemoteService *viatom_service =
        client->getService(NimBLEUUID(VIATOM_SERVICE_UUID));
    if (!viatom_service) return false;

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor Viatom service found\n");
    NimBLERemoteCharacteristic *read =
        viatom_service->getCharacteristic(NimBLEUUID(VIATOM_READ_UUID));
    if (!read || !read->canNotify() ||
        !read->subscribe(true, sensor_viatom_notify_cb)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom read subscribe failed\n");
        return false;
    }

    sensor_viatom_write =
        viatom_service->getCharacteristic(NimBLEUUID(VIATOM_WRITE_UUID));
    if (!sensor_viatom_write) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom write missing\n");
        return false;
    }

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor subscribed Viatom read\n");
    return true;
}

void sensor_viatom_on_connected() {
    if (!sensor_viatom_write) return;
    viatom_reset_rx();
    viatom_clear_pending();
    sensor_viatom_last_poll_ms = 0;
    sensor_viatom_need_time_sync = true;
}

void sensor_viatom_reset() {
    sensor_viatom_write = nullptr;
    viatom_reset_rx();
    viatom_clear_pending();
    sensor_viatom_last_poll_ms = 0;
    sensor_viatom_need_time_sync = false;
}

void sensor_viatom_poll(uint32_t now_ms) {
    if (!sensor_client || !sensor_client->isConnected() ||
        !sensor_viatom_write) {
        return;
    }

    if (sensor_viatom_pending_cmd != VIATOM_NO_PENDING_CMD &&
        static_cast<int32_t>(now_ms - sensor_viatom_pending_ms) >=
            static_cast<int32_t>(VIATOM_RESPONSE_TIMEOUT_MS)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom response timeout cmd=%s\n",
                  viatom_cmd_name(sensor_viatom_pending_cmd));
        viatom_reset_rx();
        viatom_clear_pending();
    }

    if (sensor_viatom_pending_cmd != VIATOM_NO_PENDING_CMD) return;

    if (sensor_viatom_need_time_sync) {
        if (viatom_sync_datetime(sensor_viatom_write)) {
            sensor_viatom_pending_cmd = VIATOM_CMD_CONFIG;
            sensor_viatom_pending_ms = now_ms;
        }
        sensor_viatom_need_time_sync = false;
        return;
    }

    if (static_cast<int32_t>(now_ms - sensor_viatom_last_poll_ms) <
        static_cast<int32_t>(VIATOM_SENSOR_POLL_MS)) {
        return;
    }

    if (viatom_send_command(sensor_viatom_write,
                            VIATOM_CMD_READ_SENSORS,
                            nullptr, 0, now_ms)) {
        sensor_viatom_last_poll_ms = now_ms;
    } else {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor Viatom poll write failed\n");
        sensor_viatom_last_poll_ms = now_ms;
    }
}
#endif

}  // namespace aircannect
