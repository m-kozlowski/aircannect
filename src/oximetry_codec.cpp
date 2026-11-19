#include "oximetry_codec.h"

namespace aircannect {

uint16_t encode_sfloat_int_value(int16_t value) {
    if (value < 0 || value > 0x07fd) return PLX_SFLOAT_NAN;
    return static_cast<uint16_t>(value) & 0x0fff;
}

static float decode_sfloat(uint16_t raw) {
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

    return value;
}

int16_t decode_sfloat_int_value(uint16_t raw, bool &valid) {
    const float value = decode_sfloat(raw);
    valid = value >= 0 && value <= 300;
    return valid ? static_cast<int16_t>(value + 0.5f) : -1;
}

float decode_plx_pi(const uint8_t *packet, size_t size, bool spot_check) {
    if (!packet || size < 7) return -1;
    const unsigned flags = packet[0];
    if (!(flags & (spot_check ? 0x08 : 0x10))) return -1;

    // PLXS tables 3.3 and 3.7: the optional index follows all other fields.
    const size_t offset = 5 + (spot_check
        ? 7 * bool(flags & 1) + 2 * bool(flags & 2) + 3 * bool(flags & 4)
        : 4 * bool(flags & 1) + 4 * bool(flags & 2) + 2 * bool(flags & 4) + 3 * bool(flags & 8));
    if (offset + 2 > size) return -1;
    return decode_sfloat(packet[offset] | (uint16_t(packet[offset + 1]) << 8));
}

bool decode_nonin_df19(const uint8_t *packet,
                       size_t len,
                       NoninDf19Reading &reading) {
    if (!packet || len < 10) return false;

    reading.spo2 = packet[7];
    reading.pi_percent = ((uint16_t(packet[3]) << 8) | packet[4]) / 100.0f;
    reading.pulse_bpm =
        (static_cast<uint16_t>(packet[8]) << 8) | packet[9];
    reading.valid = reading.spo2 != 127 && reading.spo2 <= 100 &&
                    reading.pulse_bpm != 511 && reading.pulse_bpm <= 321;
    return true;
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
