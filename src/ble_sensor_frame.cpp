#include "ble_sensor_frame.h"

#include "oximetry_codec.h"

namespace aircannect {

bool ble_sensor_frame_size(const uint8_t *header,
                           size_t len,
                           uint8_t marker,
                           size_t &frame_size) {
    frame_size = 0;
    if (!header || len < 7 || header[0] != marker) return false;

    const size_t payload_len = static_cast<size_t>(header[5]) |
        (static_cast<size_t>(header[6]) << 8);
    frame_size = payload_len + 8;
    return true;
}

bool decode_ble_sensor_frame(const uint8_t *frame,
                             size_t len,
                             size_t expected_size,
                             size_t min_payload_len,
                             BleSensorFrameView &view) {
    view = {};
    if (!frame || expected_size < 8 || len < expected_size ||
        frame[1] != static_cast<uint8_t>(frame[2] ^ 0xff)) {
        return false;
    }

    const size_t payload_len = expected_size - 8;
    if (payload_len < min_payload_len ||
        crc8_ccitt(frame, expected_size - 1) != frame[expected_size - 1]) {
        return false;
    }

    view.command = frame[1];
    view.payload = frame + 7;
    view.payload_len = payload_len;
    return true;
}

bool write_ble_sensor_frame_header(uint8_t *frame,
                                   size_t capacity,
                                   uint8_t marker,
                                   uint8_t command,
                                   size_t payload_len) {
    if (!frame || payload_len > 0xffff || payload_len + 8 > capacity) {
        return false;
    }

    frame[0] = marker;
    frame[1] = command;
    frame[2] = static_cast<uint8_t>(command ^ 0xff);
    frame[3] = 0;
    frame[4] = 0;
    frame[5] = static_cast<uint8_t>(payload_len & 0xff);
    frame[6] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    return true;
}

void write_ble_sensor_frame_crc(uint8_t *frame, size_t frame_size) {
    frame[frame_size - 1] = crc8_ccitt(frame, frame_size - 1);
}

}  // namespace aircannect
