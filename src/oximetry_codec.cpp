#include "oximetry_codec.h"

namespace aircannect {

uint16_t encode_sfloat_int_value(int16_t value) {
    if (value < 0 || value > 0x07fd) return PLX_SFLOAT_NAN;
    return static_cast<uint16_t>(value) & 0x0fff;
}

int16_t decode_sfloat_int_value(uint16_t raw, bool &valid) {
    valid = false;
    if (raw == PLX_SFLOAT_NAN || raw == PLX_SFLOAT_NRES ||
        raw == PLX_SFLOAT_POS_INF || raw == PLX_SFLOAT_NEG_INF ||
        raw == PLX_SFLOAT_RESERVED) {
        return -1;
    }

    int16_t mantissa = raw & 0x0fff;
    if (mantissa & 0x0800) mantissa |= 0xf000;
    int8_t exponent = static_cast<int8_t>((raw >> 12) & 0x0f);
    if (exponent & 0x08) exponent |= 0xf0;

    float value = static_cast<float>(mantissa);
    while (exponent > 0) {
        value *= 10.0f;
        exponent--;
    }
    while (exponent < 0) {
        value /= 10.0f;
        exponent++;
    }

    if (value < 0.0f || value > 300.0f) return -1;
    valid = true;
    return static_cast<int16_t>(value + 0.5f);
}

uint8_t crc8_ccitt(const uint8_t *data, size_t len, uint8_t crc) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace aircannect
