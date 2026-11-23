#include "ble_sensor_protocols.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "calendar_utils.h"
#include "debug_log.h"
#include "o2ring_recording_import.h"
#include "oximetry_codec.h"
#include "ring_bin_layout.h"

namespace aircannect {

namespace {

static constexpr uint8_t OXYII_CMD_LIVE_SAMPLES = 0x04;
static constexpr uint8_t OXYII_CMD_SETUP = 0x10;
static constexpr uint8_t OXYII_CMD_SET_TIME = 0xc0;
static constexpr uint8_t OXYII_CMD_AUTH = 0xff;
static constexpr uint8_t Info = 0xe1, List = 0xf1, Open = 0xf2, Read = 0xf3, Close = 0xf4;
using FileCommand = O2RingRecordingImport::Command;

bool history_command(uint8_t cmd) {
    return cmd == Info || (cmd >= List && cmd <= Close);
}

// Compact the wire's 16-byte name slots in the received buffer. The storage
// owner consumes the resulting list before the next notification is drained.
bool file_list(uint8_t *p, size_t len, std::string_view &ids) {
    if (!len || len != 1u + 16u * p[0]) return false;
    const size_t count = p[0];
    for (size_t i = 0; i < count; ++i) {
        const auto *slot = p + 1 + 16*i;
        if (slot[14] || slot[15] ||
            std::string_view(reinterpret_cast<const char *>(slot), 14).find_first_not_of(
                "0123456789") != std::string_view::npos) return false;
        memmove(p + 15*i, slot, 14);
        p[15*i + 14] = ',';
    }
    ids = {reinterpret_cast<const char *>(p), count * 15};
    return true;
}

int64_t ring_wall_ms(const uint8_t *p) {
    const int y = LittleEndian::get_le16(p);
    if (y < 2000 || y > 2099 || !p[2] || p[2] > 12 || !p[3] ||
        p[3] > calendar_days_in_month(y, p[2]) || p[4] > 23 || p[5] > 59 || p[6] > 59)
        return 0;
    return (calendar_days_from_civil(y, p[2], p[3])*86400 + p[4]*3600 + p[5]*60 + p[6])*1000;
}
static constexpr uint8_t OXYII_NO_PENDING_CMD = 0xfe;
static constexpr uint32_t OXYII_SENSOR_POLL_MS = 1000;
static constexpr uint32_t OXYII_RESPONSE_TIMEOUT_MS = 1500;
static constexpr uint32_t OXYII_FILE_TIMEOUT_MS = 6000;
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
        case Info: return "info";
        case List: return "file_list";
        case Open: return "file_open";
        case Read: return "file_read";
        case Close: return "file_close";
        default:
            return "unknown";
    }
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

    notify_characteristic_.store(notify, std::memory_order_release);
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
    oxyii_ = {};
}

void BleSensorProtocolEngine::oxyii_poll(uint32_t now_ms) {
    if (!client_ || !client_->isConnected() || !oxyii_.write) return;

    const auto timeout = history_command(oxyii_.pending_cmd) ?
        OXYII_FILE_TIMEOUT_MS : OXYII_RESPONSE_TIMEOUT_MS;
    if (oxyii_.pending_cmd != OXYII_NO_PENDING_CMD && now_ms - oxyii_.pending_ms >= timeout) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII response timeout cmd=%s\n",
                  oxyii_command_name(oxyii_.pending_cmd));
        if (history_command(oxyii_.pending_cmd) && oxyii_.pending_cmd != Info) {
            if (recordings_) recordings_->fail("ble_response_timeout");
            client_->disconnect(); // Some firmware stops answering F3 until reconnect.
            return;
        }
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

    if (oxyii_.need_identity) {
        oxyii_.need_identity = false;
        if (recordings_ && oxyii_send_command(Info, nullptr, 0, now_ms)) return;
    }

    if (oxyii_.need_time_sync && !history_ring_) {
        if (!oxyii_sync_datetime(now_ms)) {
            Log::logf(CAT_OXI, LOG_DEBUG,
                      "Sensor OxyII time write failed\n");
        }
        oxyii_.need_time_sync = false;
        return;
    }

    if (static_cast<int32_t>(now_ms - oxyii_.last_poll_ms) <
        static_cast<int32_t>(OXYII_SENSOR_POLL_MS)) {
        if (history_ring_ || oxyii_.file_open) oxyii_poll_history(now_ms);
        return;
    }

    if (!oxyii_send_command(OXYII_CMD_LIVE_SAMPLES, nullptr, 0, now_ms)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII poll write failed\n");
    }
    oxyii_.last_poll_ms = now_ms;
}

void BleSensorProtocolEngine::oxyii_poll_history(uint32_t now_ms) {
    auto &s = oxyii_;
    using Step = OxyiiFileStep;
    uint8_t command = Close, payload[20]{};
    size_t length = 0;

    if (s.file_step == Step::SaveClock) {
        const auto outcome = recordings_ ? recordings_->poll_clock() : OperationDisposition::Failed;
        if (outcome == OperationDisposition::Retry) return;
        s.file_step = Step::Idle;
        if (outcome == OperationDisposition::Succeeded && s.off_finger)
            oxyii_sync_datetime(now_ms);
        else if (outcome != OperationDisposition::Succeeded && recordings_)
            Log::logf(CAT_OXI, LOG_WARN, "O2Ring-S clock not set: observation not saved\n");
        return;
    }

    if (!recordings_ || !s.off_finger) {
        s.file_step = Step::Idle;
        s.wall_ms = 0;
        if (!s.file_open) return;
    } else if (s.file_step == Step::CloseForList || s.file_step == Step::Release) {
        command = Close;
    } else if (s.file_step == Step::List) {
        command = List;
        s.list_rx = LargeByteBuffer::allocate(8 + 1 + 255 * 16);
        if (!s.list_rx) {
            recordings_->fail("out_of_memory");
            s.file_step = Step::Idle;
            return;
        }
    } else if (s.file_open && !recordings_->active()) {
        s.file_step = Step::Release;
    } else if (s.file_step == Step::Footer) {
        command = Read;
        LittleEndian::put_le32(payload, s.file_size - RingBinLayout::SFooter);
        length = 4;
    } else {
        const auto work = recordings_->next(now_ms);
        if (work == FileCommand::Info || (s.need_time_sync && !recordings_->downloads_enabled())) {
            s.file_step = Step::CloseForList;
            command = s.need_time_sync ? Info : Close;
            s.need_time_sync = false;
        } else if (work == FileCommand::Open) {
            command = Open;
            memcpy(payload, recordings_->recording(), 14);
            length = sizeof(payload); // Padded name, then LE file type 0 (oximetry).
        } else if (work == FileCommand::Read) {
            command = Read;
            s.read_offset = recordings_->offset();
            LittleEndian::put_le32(payload, s.read_offset);
            length = 4;
        } else if (work != FileCommand::Close) {
            return;
        }
    }

    if (!oxyii_send_command(command, payload, length, now_ms)) {
        if (recordings_) recordings_->fail("ble_write_failed");
        client_->disconnect();
    }
}

void BleSensorProtocolEngine::oxyii_history_reply(uint8_t command, uint8_t *p, size_t len,
                                                  uint32_t rtt) {
    auto &s = oxyii_;
    using Step = OxyiiFileStep;
    if (command == Close) {
        s.file_open = false;
        if (s.file_step == Step::CloseForList) s.file_step = Step::List;
        else {
            if (s.file_step == Step::Idle && recordings_)
                recordings_->reply(FileCommand::Close, 0, nullptr, 0);
            s.file_step = Step::Idle;
        }
        return;
    }
    if (command == Open) s.file_open = true;
    if (!recordings_) return;
    if (command == Info) {
        if (len >= 38 && p[37] <= len - 38)
            recordings_->identify({reinterpret_cast<const char *>(p + 38), p[37]});
        if (!s.off_finger || s.file_step != Step::CloseForList) return;

        s.wall_ms = len >= 32 ? ring_wall_ms(p + 24) : 0;
        if (!s.wall_ms) Log::logf(CAT_OXI, LOG_DEBUG, "O2Ring-S clock not set: invalid RTC\n");
        timeval now{};
        gettimeofday(&now, nullptr);
        s.info_utc_ms = int64_t(now.tv_sec)*1000 + now.tv_usec/1000;
        s.info_rtt_ms = rtt;
        return;
    }
    if (!s.off_finger) return;
    if (command == List) {
        std::string_view ids;
        s.file_step = Step::Idle;
        if (!file_list(p, len, ids)) return recordings_->fail("invalid_recording_list");
        recordings_->listed(ids);

        if (s.wall_ms && recordings_->save_clock(ids, s.wall_ms, s.info_utc_ms, s.info_rtt_ms))
            s.file_step = Step::SaveClock;
        s.wall_ms = 0;
    } else if (command == Open) {
        s.file_size = len >= 4 ? LittleEndian::get_le32(p) : 0;
        // Size alone can precede finalization. Check the remote trailer before
        // accepting the file or resuming a partial copy, including after reboot.
        if (s.file_size >= RingBinLayout{RingBinLayout::O2RingS}.offset(1) + RingBinLayout::SFooter)
            s.file_step = Step::Footer;
        else {
            recordings_->skip_open(true);
            s.file_step = Step::Release;
        }
    } else if (s.file_step == Step::Footer) {
        if (RingBinLayout::s_footer_matches(p, len, s.file_size)) {
            recordings_->opened(s.file_size, O2RingRecordingImport::Addressing::Bytes);
            s.file_step = Step::Idle;
        } else {
            recordings_->skip_open(true);
            s.file_step = Step::Release;
        }
    } else {
        recordings_->received(s.read_offset, p, len);
    }
}

void BleSensorProtocolEngine::oxyii_notify_callback(
    NimBLERemoteCharacteristic *characteristic,
    uint8_t *data,
    size_t len,
    bool is_notify) {
    (void)is_notify;
    BleSensorProtocolEngine *engine =
        active_.load(std::memory_order_acquire);
    if (engine) {
        (void)engine->enqueue_notification(
            ActiveProtocol::Oxyii, characteristic, data, len);
    }
}

void BleSensorProtocolEngine::oxyii_notify(const uint8_t *data, size_t len) {
    auto *rx = oxyii_.list_rx ? oxyii_.list_rx->data() : oxyii_.rx;
    const auto capacity = oxyii_.list_rx ? oxyii_.list_rx->size() : sizeof(oxyii_.rx);
    const auto result = append_ble_sensor_frame(
        data, len, 0xa5, rx, capacity,
        oxyii_.rx_len, oxyii_.rx_want);

    if (result == BleSensorFrameResult::Incomplete) return;
    if (result == BleSensorFrameResult::Invalid) {
        Log::logf(CAT_OXI, LOG_DEBUG, "Sensor OxyII RX invalid fragment\n");
        oxyii_reset_rx();
        return;
    }

    BleSensorFrameView frame;
    if (!decode_ble_sensor_frame(rx, oxyii_.rx_len,
                                 oxyii_.rx_want, 0, frame)) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII RX decode failed len=%u\n",
                  static_cast<unsigned>(oxyii_.rx_len));
        oxyii_reset_rx();
        return;
    }
    const uint8_t command = frame.command;
    oxyii_reset_rx();

    const uint8_t pending_command = oxyii_.pending_cmd;
    if (history_command(command)) {
        if (command != pending_command || rx[3] != 1 ||
            rx[4] != uint8_t(oxyii_.sequence - 1)) return;
        const auto rtt = millis() - oxyii_.pending_ms;
        const auto owner = std::move(oxyii_.list_rx);
        oxyii_clear_pending();
        oxyii_history_reply(command, rx + 7, frame.payload_len, rtt);
        return;
    }
    if (pending_command == OXYII_CMD_SETUP && command == OXYII_CMD_SETUP) {
        oxyii_clear_pending();
        oxyii_.need_identity = history_ring_;
        oxyii_.need_time_sync = true;
        return;
    }
    if (pending_command == OXYII_CMD_SET_TIME &&
        command == OXYII_CMD_SET_TIME) {
        oxyii_clear_pending();
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

    if (frame.payload_len < 10) {
        Log::logf(CAT_OXI, LOG_DEBUG,
                  "Sensor OxyII live decode failed len=%u\n",
                  static_cast<unsigned>(frame.payload_len));
        return;
    }

    oxyii_clear_pending();
    const uint8_t *p = frame.payload;
    const uint8_t spo2 = p[6];
    const uint16_t pulse = uint16_t(p[8]) | (uint16_t(p[9]) << 8);
    const bool have_o2 = spo2 > 0 && spo2 <= 100;
    const bool have_pulse = pulse > 0 && pulse < 250;
    const bool valid = have_o2 && have_pulse;

    // Off-finger placeholders are not measurements. Keep motion during
    // poor signal if the sensor still reports contact (RtParam.sensorState).
    const bool charging = frame.payload_len >= 13 && (p[12] == 1 || p[12] == 2);
    const bool evidence = have_o2 || have_pulse || (p[5] != 0 &&
        !(p[5] == 3 && oxyii_.file_open && oxyii_.off_finger));
    const bool contact = observe_wear(evidence, charging, millis());
    oxyii_.off_finger = charging || !contact;
    const float pi = contact ? p[7] / 10.0f : -1.0f;
    const int16_t motion = contact && frame.payload_len >= 12 ? p[11] : -1;

    Log::logf(CAT_OXI, LOG_DEBUG,
              "Sensor OxyII reading %s spo2=%u pulse=%u\n",
              valid ? "valid" : "invalid",
              static_cast<unsigned>(spo2),
              static_cast<unsigned>(pulse));

    emit_sample(have_o2 ? encode_sfloat_int_value(spo2) : PLX_SFLOAT_NAN,
                have_pulse ? encode_sfloat_int_value(pulse) : PLX_SFLOAT_NAN,
                !valid, history_ring_, contact, history_ring_ && charging, pi, motion);
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
    if (!oxyii_.write) return false;

    uint8_t frame[OXYII_TX_MAX] = {};
    if (!write_ble_sensor_frame_header(frame, sizeof(frame), 0xa5,
                                       command, payload_len)) {
        return false;
    }
    const size_t frame_len = payload_len + 8;
    frame[4] = sequence;
    if (payload && payload_len) memcpy(frame + 7, payload, payload_len);
    write_ble_sensor_frame_crc(frame, frame_len);
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
