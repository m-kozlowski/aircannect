#include "oximetry_sensor_protocols.h"

#include <string.h>
#include <time.h>

#include "debug_log.h"

namespace aircannect {

namespace {

static constexpr uint8_t OXYII_CMD_LIVE_SAMPLES = 0x04;
static constexpr uint8_t OXYII_CMD_SETUP = 0x10;
static constexpr uint8_t OXYII_CMD_SET_TIME = 0xc0;
static constexpr uint8_t OXYII_CMD_AUTH = 0xff;
static constexpr uint8_t OXYII_NO_PENDING_CMD = 0xfe;
static constexpr uint32_t OXYII_SENSOR_POLL_MS = 1000;
static constexpr uint32_t OXYII_RESPONSE_TIMEOUT_MS = 1500;
static constexpr size_t OXYII_RX_MAX = 640;
static constexpr size_t OXYII_TX_MAX = 32;

static constexpr uint8_t LEPUCLOUD_MD5[16] = {
    0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
    0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
};

#if AC_OXIMETRY_BLE_ENABLED
NimBLERemoteCharacteristic *sensor_oxyii_write = nullptr;
uint8_t sensor_oxyii_rx[OXYII_RX_MAX] = {};
size_t sensor_oxyii_rx_len = 0;
size_t sensor_oxyii_rx_want = 0;
uint8_t sensor_oxyii_pending_cmd = OXYII_NO_PENDING_CMD;
uint32_t sensor_oxyii_pending_ms = 0;
uint32_t sensor_oxyii_last_poll_ms = 0;
uint8_t sensor_oxyii_seq = 0;
bool sensor_oxyii_need_auth = false;
bool sensor_oxyii_need_setup = false;
bool sensor_oxyii_need_time_sync = false;
#endif

void oxyii_reset_rx() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_oxyii_rx_len = 0;
    sensor_oxyii_rx_want = 0;
#endif
}

void oxyii_clear_pending() {
#if AC_OXIMETRY_BLE_ENABLED
    sensor_oxyii_pending_cmd = OXYII_NO_PENDING_CMD;
    sensor_oxyii_pending_ms = 0;
#endif
}

const char *oxyii_cmd_name(uint8_t cmd) {
    switch (cmd) {
        case OXYII_CMD_LIVE_SAMPLES: return "live_samples";
        case OXYII_CMD_SETUP: return "setup";
        case OXYII_CMD_SET_TIME: return "set_time";
        case OXYII_CMD_AUTH: return "auth";
        case OXYII_NO_PENDING_CMD: return "none";
        default: return "unknown";
    }
}

void log_oxyii_hex_debug(const char *label,
                         const uint8_t *data,
                         size_t len,
                         uint8_t cmd = OXYII_NO_PENDING_CMD) {
    if (Log::get_cat_level(CAT_OXI) < LOG_DEBUG) return;

    static constexpr size_t MAX_BYTES = 24;
    const size_t shown = len < MAX_BYTES ? len : MAX_BYTES;
    char hex[MAX_BYTES * 3 + 1] = {};
    size_t pos = 0;
    for (size_t i = 0; data && i < shown && pos + 4 < sizeof(hex); ++i) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
    }
    if (pos > 0) hex[pos - 1] = 0;
    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] %s len=%u pending=%s%s %s\n",
              label ? label : "OxyII",
              static_cast<unsigned>(len),
              oxyii_cmd_name(cmd),
              len > shown ? " truncated" : "",
              hex);
}

bool oxyii_decode_frame(const uint8_t *frame,
                        size_t len,
                        uint8_t &cmd,
                        const uint8_t *&payload,
                        size_t &payload_len) {
    if (!frame || len < 8 || frame[0] != 0xa5) return false;
    cmd = frame[1];
    if (frame[2] != static_cast<uint8_t>(~cmd)) return false;

    payload_len =
        static_cast<uint16_t>(frame[5]) |
        (static_cast<uint16_t>(frame[6]) << 8);
    const size_t total = payload_len + 8;
    if (len < total) return false;
    if (crc8_ccitt(frame, total - 1) != frame[total - 1]) return false;

    payload = frame + 7;
    return true;
}

bool oxyii_decode_reading(const uint8_t *payload,
                          size_t len,
                          uint16_t &spo2_raw,
                          uint16_t &pulse_raw,
                          bool &invalid) {
    if (!payload || len < 9) return false;

    const uint8_t spo2 = payload[6];
    const uint8_t pulse = payload[8];
    const bool valid =
        spo2 > 0 && spo2 <= 100 && pulse > 0 && pulse != 0xff &&
        pulse < 250;
    spo2_raw = valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN;
    pulse_raw = valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN;
    invalid = !valid;
    return true;
}

bool oxyii_write_frame(NimBLERemoteCharacteristic *write_chr,
                       uint8_t cmd,
                       const uint8_t *payload,
                       size_t payload_len,
                       uint8_t seq) {
    if (!write_chr || payload_len > 0xffff) return false;
    const size_t frame_len = 7 + payload_len + 1;
    if (frame_len > OXYII_TX_MAX) return false;

    uint8_t frame[OXYII_TX_MAX] = {};
    frame[0] = 0xa5;
    frame[1] = cmd;
    frame[2] = static_cast<uint8_t>(~cmd);
    frame[3] = 0x00;
    frame[4] = seq;
    frame[5] = static_cast<uint8_t>(payload_len & 0xff);
    frame[6] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    if (payload && payload_len) {
        memcpy(frame + 7, payload, payload_len);
    }
    frame[7 + payload_len] = crc8_ccitt(frame, 7 + payload_len);
    return write_chr->writeValue(frame, frame_len, false);
}

bool oxyii_send_command(NimBLERemoteCharacteristic *write_chr,
                        uint8_t cmd,
                        const uint8_t *payload,
                        size_t payload_len,
                        uint32_t now_ms,
                        bool expect_reply = true) {
    const uint8_t seq = sensor_oxyii_seq++;
    if (!oxyii_write_frame(write_chr, cmd, payload, payload_len, seq)) {
        return false;
    }
    if (expect_reply) {
        sensor_oxyii_pending_cmd = cmd;
        sensor_oxyii_pending_ms = now_ms;
    }
    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] Sensor OxyII TX cmd=%s payload_len=%u\n",
              oxyii_cmd_name(cmd),
              static_cast<unsigned>(payload_len));
    return true;
}

bool oxyii_send_auth(NimBLERemoteCharacteristic *write_chr,
                     uint32_t now_ms) {
    (void)now_ms;
    const time_t seconds = time(nullptr);
    if (seconds < 1704067200) return false;  // 2024-01-01T00:00:00Z

    uint8_t session_key[16] = {};
    for (size_t i = 0; i < 8; ++i) {
        session_key[i] = LEPUCLOUD_MD5[i * 2];
    }
    session_key[8] = '0';
    session_key[9] = '0';
    session_key[10] = '0';
    session_key[11] = '0';
    const uint32_t ts = static_cast<uint32_t>(seconds);
    // OxyII expects bit shifts 0,1,2,3 here, not byte shifts.
    for (uint8_t i = 0; i < 4; ++i) {
        session_key[12 + i] = static_cast<uint8_t>((ts >> i) & 0xff);
    }

    uint8_t payload[16] = {};
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = session_key[i] ^ LEPUCLOUD_MD5[i];
    }

    return oxyii_send_command(write_chr,
                              OXYII_CMD_AUTH,
                              payload,
                              sizeof(payload),
                              now_ms,
                              false);
}

bool oxyii_sync_datetime(NimBLERemoteCharacteristic *write_chr,
                         uint32_t now_ms) {
    const time_t now = time(nullptr);
    if (now < 1704067200) return false;  // 2024-01-01T00:00:00Z

    struct tm local = {};
    localtime_r(&now, &local);

    uint8_t payload[8] = {};
    const uint16_t year = static_cast<uint16_t>(local.tm_year + 1900);
    payload[0] = static_cast<uint8_t>(year & 0xff);
    payload[1] = static_cast<uint8_t>((year >> 8) & 0xff);
    payload[2] = static_cast<uint8_t>(local.tm_mon + 1);
    payload[3] = static_cast<uint8_t>(local.tm_mday);
    payload[4] = static_cast<uint8_t>(local.tm_hour);
    payload[5] = static_cast<uint8_t>(local.tm_min);
    payload[6] = static_cast<uint8_t>(local.tm_sec);
    payload[7] = 0;

    return oxyii_send_command(write_chr,
                              OXYII_CMD_SET_TIME,
                              payload,
                              sizeof(payload),
                              now_ms);
}

void sensor_oxyii_notify_cb(NimBLERemoteCharacteristic *chr,
                            uint8_t *data,
                            size_t len,
                            bool is_notify) {
    (void)chr;
    (void)is_notify;
    if (!data || !len) return;
    log_oxyii_hex_debug("Sensor OxyII RX fragment",
                        data,
                        len,
                        sensor_oxyii_pending_cmd);

    if (data[0] == 0xa5) {
        sensor_oxyii_rx_len = 0;
        sensor_oxyii_rx_want = 0;
        if (len >= 7) {
            const uint16_t payload_len =
                static_cast<uint16_t>(data[5]) |
                (static_cast<uint16_t>(data[6]) << 8);
            const size_t want = static_cast<size_t>(payload_len) + 8;
            if (want <= sizeof(sensor_oxyii_rx)) {
                sensor_oxyii_rx_want = want;
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "[OXI] Sensor OxyII RX too large want=%u\n",
                          static_cast<unsigned>(want));
                return;
            }
        }
    }

    if (!sensor_oxyii_rx_want ||
        sensor_oxyii_rx_len + len > sizeof(sensor_oxyii_rx)) {
        oxyii_reset_rx();
        return;
    }

    memcpy(sensor_oxyii_rx + sensor_oxyii_rx_len, data, len);
    sensor_oxyii_rx_len += len;
    if (sensor_oxyii_rx_len < sensor_oxyii_rx_want) return;

    uint8_t cmd = 0;
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
    if (!oxyii_decode_frame(sensor_oxyii_rx,
                            sensor_oxyii_rx_len,
                            cmd,
                            payload,
                            payload_len)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor OxyII RX decode failed len=%u\n",
                  static_cast<unsigned>(sensor_oxyii_rx_len));
        oxyii_reset_rx();
        return;
    }
    oxyii_reset_rx();

    const uint8_t pending_cmd = sensor_oxyii_pending_cmd;
    oxyii_clear_pending();
    if (pending_cmd == OXYII_CMD_SETUP && cmd == OXYII_CMD_SETUP) {
        sensor_oxyii_need_time_sync = true;
        return;
    }
    if (pending_cmd == OXYII_CMD_SET_TIME && cmd == OXYII_CMD_SET_TIME) {
        return;
    }
    if (pending_cmd != OXYII_CMD_LIVE_SAMPLES ||
        cmd != OXYII_CMD_LIVE_SAMPLES) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor OxyII RX ignored cmd=%s pending=%s\n",
                  oxyii_cmd_name(cmd),
                  oxyii_cmd_name(pending_cmd));
        return;
    }

    uint16_t spo2_raw = PLX_SFLOAT_NAN;
    uint16_t pulse_raw = PLX_SFLOAT_NAN;
    bool invalid = true;
    if (!oxyii_decode_reading(payload, payload_len, spo2_raw, pulse_raw,
                              invalid)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor OxyII live decode failed len=%u\n",
                  static_cast<unsigned>(payload_len));
        return;
    }
    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] Sensor OxyII reading %s\n",
              invalid ? "invalid" : "valid");

    if (sensor_owner) {
        sensor_owner->on_sensor_sample(spo2_raw, pulse_raw, invalid);
    }
}

}  // namespace

#if AC_OXIMETRY_BLE_ENABLED
bool sensor_subscribe_oxyii(NimBLEClient *client) {
    if (!client) return false;
    NimBLERemoteService *oxyii_service =
        client->getService(NimBLEUUID(OXYII_SERVICE_UUID));
    if (!oxyii_service) return false;

    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] Sensor OxyII service found\n");
    NimBLERemoteCharacteristic *notify =
        oxyii_service->getCharacteristic(NimBLEUUID(OXYII_NOTIFY_UUID));
    if (!notify || !notify->canNotify() ||
        !notify->subscribe(true, sensor_oxyii_notify_cb)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor OxyII notify subscribe failed\n");
        return false;
    }

    sensor_oxyii_write =
        oxyii_service->getCharacteristic(NimBLEUUID(OXYII_WRITE_UUID));
    if (!sensor_oxyii_write) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor OxyII write missing\n");
        return false;
    }

    Log::logf(CAT_OXI, LOG_DEBUG,
              "[OXI] Sensor subscribed OxyII notify\n");
    return true;
}

void sensor_oxyii_on_connected() {
    if (!sensor_oxyii_write) return;
    oxyii_reset_rx();
    oxyii_clear_pending();
    sensor_oxyii_last_poll_ms = 0;
    sensor_oxyii_seq = 0;
    sensor_oxyii_need_auth = true;
    sensor_oxyii_need_setup = false;
    sensor_oxyii_need_time_sync = false;
}

void sensor_oxyii_reset() {
    sensor_oxyii_write = nullptr;
    oxyii_reset_rx();
    oxyii_clear_pending();
    sensor_oxyii_last_poll_ms = 0;
    sensor_oxyii_seq = 0;
    sensor_oxyii_need_auth = false;
    sensor_oxyii_need_setup = false;
    sensor_oxyii_need_time_sync = false;
}

void sensor_oxyii_poll(uint32_t now_ms) {
    if (!sensor_client || !sensor_client->isConnected() ||
        !sensor_oxyii_write) {
        return;
    }

    if (sensor_oxyii_pending_cmd != OXYII_NO_PENDING_CMD &&
        static_cast<int32_t>(now_ms - sensor_oxyii_pending_ms) >=
            static_cast<int32_t>(OXYII_RESPONSE_TIMEOUT_MS)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor OxyII response timeout cmd=%s\n",
                  oxyii_cmd_name(sensor_oxyii_pending_cmd));
        oxyii_reset_rx();
        oxyii_clear_pending();
    }

    if (sensor_oxyii_pending_cmd != OXYII_NO_PENDING_CMD) return;

    if (sensor_oxyii_need_auth) {
        if (oxyii_send_auth(sensor_oxyii_write, now_ms)) {
            sensor_oxyii_need_auth = false;
            sensor_oxyii_need_setup = true;
        } else {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor OxyII auth write failed\n");
        }
        return;
    }

    if (sensor_oxyii_need_setup) {
        const uint8_t payload = 0;
        if (oxyii_send_command(sensor_oxyii_write,
                               OXYII_CMD_SETUP,
                               &payload,
                               sizeof(payload),
                               now_ms)) {
            sensor_oxyii_need_setup = false;
        } else {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor OxyII setup write failed\n");
        }
        return;
    }

    if (sensor_oxyii_need_time_sync) {
        if (oxyii_sync_datetime(sensor_oxyii_write, now_ms)) {
            sensor_oxyii_need_time_sync = false;
        } else {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "[OXI] Sensor OxyII time write failed\n");
            sensor_oxyii_need_time_sync = false;
        }
        return;
    }

    if (static_cast<int32_t>(now_ms - sensor_oxyii_last_poll_ms) <
        static_cast<int32_t>(OXYII_SENSOR_POLL_MS)) {
        return;
    }

    if (oxyii_send_command(sensor_oxyii_write,
                           OXYII_CMD_LIVE_SAMPLES,
                           nullptr,
                           0,
                           now_ms)) {
        sensor_oxyii_last_poll_ms = now_ms;
    } else {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "[OXI] Sensor OxyII poll write failed\n");
        sensor_oxyii_last_poll_ms = now_ms;
    }
}
#endif

}  // namespace aircannect
