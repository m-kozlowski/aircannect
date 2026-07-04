#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {
namespace report_records_detail {

static constexpr size_t SERIES_V2_HEADER_SIZE = 24;
static constexpr size_t EVENT_RECORD_WIRE_SIZE = 16;
static constexpr uint32_t SERIES_V2_MAGIC = 0x32535241u;  // "ARS2"
static constexpr uint32_t SERIES_V2_MODE_UNIFORM = 1;
static constexpr uint32_t SERIES_V2_MODE_EXPLICIT = 2;
static constexpr uint32_t SERIES_V2_VALUE_INT32_MILLI = 1;

inline void put_le16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

inline void put_le32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

inline void put_le64(uint8_t *out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
    }
}

inline uint16_t get_le16(const uint8_t *in) {
    return static_cast<uint16_t>(in[0]) |
           (static_cast<uint16_t>(in[1]) << 8);
}

inline uint32_t get_le32(const uint8_t *in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

inline uint64_t get_le64(const uint8_t *in) {
    uint64_t value = 0;

    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(in[i]) << (i * 8);
    }

    return value;
}

inline bool valid_timestamp(int64_t timestamp_ms) {
    return timestamp_ms > 0;
}

}  // namespace report_records_detail
}  // namespace aircannect
