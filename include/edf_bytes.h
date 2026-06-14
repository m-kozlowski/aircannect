#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

inline bool edf_bit_get(const uint8_t *bits, size_t index) {
    if (!bits) return false;
    return (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

inline void edf_bit_set(uint8_t *bits, size_t index, bool value = true) {
    if (!bits) return;
    const uint8_t mask = static_cast<uint8_t>(1u << (index % 8));
    if (value) bits[index / 8] |= mask;
    else bits[index / 8] &= static_cast<uint8_t>(~mask);
}

inline int16_t edf_read_i16_le(const uint8_t *src) {
    const uint16_t raw =
        static_cast<uint16_t>(src[0]) |
        (static_cast<uint16_t>(src[1]) << 8);
    return static_cast<int16_t>(raw);
}

inline int16_t edf_read_i16_le_sample(const uint8_t *bytes,
                                      size_t sample_offset) {
    return edf_read_i16_le(bytes + sample_offset * 2);
}

inline void edf_write_i16_le(uint8_t *dst, int16_t value) {
    const uint16_t raw = static_cast<uint16_t>(value);
    dst[0] = static_cast<uint8_t>(raw & 0xff);
    dst[1] = static_cast<uint8_t>((raw >> 8) & 0xff);
}

inline void edf_write_i16_le_sample(uint8_t *bytes,
                                    size_t sample_offset,
                                    int16_t value) {
    edf_write_i16_le(bytes + sample_offset * 2, value);
}

inline bool edf_append_i16_le(uint8_t *dst,
                              size_t capacity,
                              size_t &offset,
                              int16_t value) {
    if (!dst || offset + 2 > capacity) return false;
    edf_write_i16_le(dst + offset, value);
    offset += 2;
    return true;
}

}  // namespace aircannect
