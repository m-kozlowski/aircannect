#include "ble_sensor_protocols.h"

#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "oximetry_codec.h"

namespace aircannect {

namespace {

static constexpr uint8_t OXYII_CMD_LIVE_SAMPLES = 0x04;
static constexpr uint8_t OXYII_CMD_SETUP = 0x10;
static constexpr uint8_t OXYII_CMD_SET_TIME = 0xc0;
static constexpr uint8_t OXYII_CMD_AUTH = 0xff;
static constexpr uint8_t OXYII_NO_PENDING_CMD = 0xfe;
static constexpr uint32_t OXYII_SENSOR_POLL_MS = 1000;
static constexpr uint32_t OXYII_RESPONSE_TIMEOUT_MS = 1500;
static constexpr size_t OXYII_TX_MAX = 32;

static constexpr uint8_t LEPUCLOUD_MD5[16] = {
    0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
    0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
};

const char *oxyii_command_name(uint8_t command) {
    switch (command) {
        case OXYII_CMD_LIVE_SAMPLES:
            return "live_samples";
        case OXYII_CMD_SETUP:
            return "setup";
        case OXYII_CMD_SET_TIME:
            return "set_time";
        case OXYII_CMD_AUTH:
            return "auth";
        case OXYII_NO_PENDING_CMD:
            return "none";
        default:
            return "unknown";
    }
}

bool decode_oxyii_frame(const uint8_t *frame,
                        size_t len,
                        uint8_t &command,
                        const uint8_t *&payload,
                        size_t &payload_len) {
    if (!frame || len < 8 || frame[0] != 0xa5) return false;

    command = frame[1];
    if (frame[2] != static_cast<uint8_t>(~command)) return false;

    payload_len = static_cast<uint16_t>(frame[5]) |
                  (static_cast<uint16_t>(frame[6]) << 8);
    const size_t total = payload_len + 8;
    if (len < total) return false;
    if (crc8_ccitt(frame, total - 1) != frame[total - 1]) return false;

    payload = frame + 7;
    return true;
}

bool decode_oxyii_reading(const uint8_t *payload,
                          size_t len,
                          uint8_t &spo2,
                          uint8_t &pulse,
                          uint16_t &spo2_raw,
                          uint16_t &pulse_raw,
                          bool &invalid) {
    if (!payload || len < 9) return false;

    spo2 = payload[6];
    pulse = payload[8];
    const bool valid = spo2 > 0 && spo2 <= 100 && pulse > 0 &&
                       pulse != 0xff && pulse < 250;
    spo2_raw = valid ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN;
    pulse_raw = valid ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN;
    invalid = !valid;
    return true;
}

}  // namespace

#if AC_OXIMETRY_BLE_ENABLED
bool BleSensorProtocolEngine::subscribe_oxyii() {
    if (!client_) return false;

    NimBLERemoteService *service = client_->getService(NimBLEUUID(
        "E8FB0001-A14B-98F9-831B-4E2941D01248"));
    if (!service) return false;

    Log::logf(CAT_OXI, LOG_DEBUG, "Sensor OxyII service found\n");
    NimBLERemoteCharacteristic *notify =
        service->getCharacteristic(NimBLEUUID(
            "E8FB0003-A14B-98F9-831B-4E2941D01248"));
    if (!notify || !notify->canNotify() ||
        !notify->subscribe(true, oxyii_notify_callback)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII notify subscribe failed\n");
        return false;
    }

    oxyii_.write = service->getCharacteristic(NimBLEUUID(
        "E8FB0002-A14B-98F9-831B-4E2941D01248"));
    if (!oxyii_.write) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Sensor OxyII write missing\n");
        return false;
    }

    Log::logf(CAT_OXI, LOG_DEBUG, "Sensor subscribed OxyII notify\n");
    return true;
}

void BleSensorProtocolEngine::oxyii_on_connected() {
    if (!oxyii_.write) return;

    oxyii_reset_rx();
    oxyii_clear_pending();
    oxyii_.last_poll_ms = 0;
    oxyii_.sequence = 0;
    oxyii_.need_auth = true;
    oxyii_.need_setup = false;
    oxyii_.need_time_sync = false;
}

void BleSensorProtocolEngine::oxyii_reset() {
    oxyii_.write = nullptr;
    oxyii_reset_rx();
    oxyii_clear_pending();
    oxyii_.last_poll_ms = 0;
    oxyii_.sequence = 0;
    oxyii_.need_auth = false;
    oxyii_.need_setup = false;
    oxyii_.need_time_sync = false;
}

void BleSensorProtocolEngine::oxyii_poll(uint32_t now_ms) {
    if (!client_ || !client_->isConnected() || !oxyii_.write) return;

    if (oxyii_.pending_cmd != OXYII_NO_PENDING_CMD &&
        static_cast<int32_t>(now_ms - oxyii_.pending_ms) >=
            static_cast<int32_t>(OXYII_RESPONSE_TIMEOUT_MS)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII response timeout cmd=%s\n",
                  oxyii_command_name(oxyii_.pending_cmd));
        oxyii_reset_rx();
        oxyii_clear_pending();
    }
    if (oxyii_.pending_cmd != OXYII_NO_PENDING_CMD) return;

    if (oxyii_.need_auth) {
        if (oxyii_send_auth(now_ms)) {
            oxyii_.need_auth = false;
            oxyii_.need_setup = true;
        } else {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor OxyII auth write failed\n");
        }
        return;
    }

    if (oxyii_.need_setup) {
        const uint8_t payload = 0;
        if (oxyii_send_command(OXYII_CMD_SETUP, &payload,
                               sizeof(payload), now_ms)) {
            oxyii_.need_setup = false;
        } else {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor OxyII setup write failed\n");
        }
        return;
    }

    if (oxyii_.need_time_sync) {
        if (!oxyii_sync_datetime(now_ms)) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor OxyII time write failed\n");
        }
        oxyii_.need_time_sync = false;
        return;
    }

    if (static_cast<int32_t>(now_ms - oxyii_.last_poll_ms) <
        static_cast<int32_t>(OXYII_SENSOR_POLL_MS)) {
        return;
    }

    if (!oxyii_send_command(OXYII_CMD_LIVE_SAMPLES, nullptr, 0, now_ms)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII poll write failed\n");
    }
    oxyii_.last_poll_ms = now_ms;
}

void BleSensorProtocolEngine::oxyii_notify_callback(
    NimBLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t len,
    bool is_notify) {
    (void)characteristic;
    (void)is_notify;
    BleSensorProtocolEngine *engine =
        active_.load(std::memory_order_acquire);
    if (engine) engine->oxyii_notify(data, len);
}

void BleSensorProtocolEngine::oxyii_notify(const uint8_t *data, size_t len) {
    if (!data || !len) return;

    if (data[0] == 0xa5) {
        oxyii_reset_rx();
        if (len >= 7) {
            const uint16_t payload_len =
                static_cast<uint16_t>(data[5]) |
                (static_cast<uint16_t>(data[6]) << 8);
            const size_t wanted = static_cast<size_t>(payload_len) + 8;
            if (wanted <= sizeof(oxyii_.rx)) {
                oxyii_.rx_want = wanted;
            } else {
                Log::logf(CAT_OXI, LOG_DEBUG,
                          "Sensor OxyII RX too large want=%u\n",
                          static_cast<unsigned>(wanted));
                return;
            }
        }
    }

    if (!oxyii_.rx_want ||
        oxyii_.rx_len + len > sizeof(oxyii_.rx)) {
        oxyii_reset_rx();
        return;
    }

    memcpy(oxyii_.rx + oxyii_.rx_len, data, len);
    oxyii_.rx_len += len;
    if (oxyii_.rx_len < oxyii_.rx_want) return;

    uint8_t command = 0;
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
    if (!decode_oxyii_frame(oxyii_.rx, oxyii_.rx_len, command,
                            payload, payload_len)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII RX decode failed len=%u\n",
                  static_cast<unsigned>(oxyii_.rx_len));
        oxyii_reset_rx();
        return;
    }
    oxyii_reset_rx();

    const uint8_t pending_command = oxyii_.pending_cmd;
    oxyii_clear_pending();
    if (pending_command == OXYII_CMD_SETUP && command == OXYII_CMD_SETUP) {
        oxyii_.need_time_sync = true;
        return;
    }
    if (pending_command == OXYII_CMD_SET_TIME &&
        command == OXYII_CMD_SET_TIME) {
        return;
    }
    if (pending_command != OXYII_CMD_LIVE_SAMPLES ||
        command != OXYII_CMD_LIVE_SAMPLES) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII RX ignored cmd=%s pending=%s\n",
                  oxyii_command_name(command),
                  oxyii_command_name(pending_command));
        return;
    }

    uint16_t spo2_raw = PLX_SFLOAT_NAN;
    uint16_t pulse_raw = PLX_SFLOAT_NAN;
    uint8_t spo2 = 0;
    uint8_t pulse = 0;
    bool invalid = true;
    if (!decode_oxyii_reading(payload, payload_len, spo2, pulse,
                              spo2_raw, pulse_raw, invalid)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII live decode failed len=%u\n",
                  static_cast<unsigned>(payload_len));
        return;
    }

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor OxyII reading %s spo2=%u pulse=%u\n",
              invalid ? "invalid" : "valid",
              static_cast<unsigned>(spo2),
              static_cast<unsigned>(pulse));
    emit_sample(spo2_raw, pulse_raw, invalid);
}

void BleSensorProtocolEngine::oxyii_reset_rx() {
    oxyii_.rx_len = 0;
    oxyii_.rx_want = 0;
}

void BleSensorProtocolEngine::oxyii_clear_pending() {
    oxyii_.pending_cmd = OXYII_NO_PENDING_CMD;
    oxyii_.pending_ms = 0;
}

bool BleSensorProtocolEngine::oxyii_write_frame(
    uint8_t command,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t sequence) {
    if (!oxyii_.write || payload_len > 0xffff) return false;

    const size_t frame_len = 7 + payload_len + 1;
    if (frame_len > OXYII_TX_MAX) return false;

    uint8_t frame[OXYII_TX_MAX] = {};
    frame[0] = 0xa5;
    frame[1] = command;
    frame[2] = static_cast<uint8_t>(~command);
    frame[4] = sequence;
    frame[5] = static_cast<uint8_t>(payload_len & 0xff);
    frame[6] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    if (payload && payload_len) memcpy(frame + 7, payload, payload_len);
    frame[7 + payload_len] = crc8_ccitt(frame, 7 + payload_len);
    return oxyii_.write->writeValue(frame, frame_len, false);
}

bool BleSensorProtocolEngine::oxyii_send_command(
    uint8_t command,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms,
    bool expect_reply) {
    const uint8_t sequence = oxyii_.sequence++;
    if (!oxyii_write_frame(command, payload, payload_len, sequence)) {
        return false;
    }

    if (expect_reply) {
        oxyii_.pending_cmd = command;
        oxyii_.pending_ms = now_ms;
    }
    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor OxyII TX cmd=%s payload_len=%u\n",
              oxyii_command_name(command),
              static_cast<unsigned>(payload_len));
    return true;
}

bool BleSensorProtocolEngine::oxyii_send_auth(uint32_t now_ms) {
    const time_t seconds = time(nullptr);
    if (seconds < 1704067200) return false;

    uint8_t session_key[16] = {};
    for (size_t i = 0; i < 8; ++i) {
        session_key[i] = LEPUCLOUD_MD5[i * 2];
    }
    session_key[8] = '0';
    session_key[9] = '0';
    session_key[10] = '0';
    session_key[11] = '0';

    const uint32_t timestamp = static_cast<uint32_t>(seconds);
    // The device expects bit shifts 0, 1, 2, and 3 here.
    for (uint8_t i = 0; i < 4; ++i) {
        session_key[12 + i] =
            static_cast<uint8_t>((timestamp >> i) & 0xff);
    }

    uint8_t payload[16] = {};
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = session_key[i] ^ LEPUCLOUD_MD5[i];
    }
    return oxyii_send_command(OXYII_CMD_AUTH, payload, sizeof(payload),
                              now_ms, false);
}

bool BleSensorProtocolEngine::oxyii_sync_datetime(uint32_t now_ms) {
    const time_t now = time(nullptr);
    if (now < 1704067200) return false;

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

    return oxyii_send_command(OXYII_CMD_SET_TIME, payload,
                              sizeof(payload), now_ms);
}
#endif

}  // namespace aircannect
