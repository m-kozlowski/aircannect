#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

struct BleSensorFrameView {
    uint8_t command = 0;
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
};

enum class BleSensorFrameResult { Incomplete, Complete, Invalid };

// Keep used/expected between notifications; reset both after Complete/Invalid
// or a response timeout. Marker-valued continuation bytes are ordinary data.
BleSensorFrameResult append_ble_sensor_frame(
    const uint8_t *data, size_t len, uint8_t marker,
    uint8_t *buffer, size_t capacity, size_t &used, size_t &expected);

bool ble_sensor_frame_size(const uint8_t *header,
                           size_t len,
                           uint8_t marker,
                           size_t &frame_size);
// expected_size must come from ble_sensor_frame_size() for this same frame.
bool decode_ble_sensor_frame(const uint8_t *frame,
                             size_t len,
                             size_t expected_size,
                             size_t min_payload_len,
                             BleSensorFrameView &view);
bool write_ble_sensor_frame_header(uint8_t *frame,
                                   size_t capacity,
                                   uint8_t marker,
                                   uint8_t command,
                                   size_t payload_len);
void write_ble_sensor_frame_crc(uint8_t *frame, size_t frame_size);

}  // namespace aircannect
