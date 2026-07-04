#include "report_parser_series_internal.h"

#include <limits.h>
#include <stdio.h>

namespace aircannect {

ReportSeriesBitReader::ReportSeriesBitReader(const uint8_t *data, size_t len)
    : data_(data), len_(len) {}

bool ReportSeriesBitReader::read_bit(uint8_t &out) {
    if (!data_ || byte_index_ >= len_) return false;

    out = static_cast<uint8_t>(
        (data_[byte_index_] >> (7 - bit_index_)) & 1u);

    bit_index_++;
    if (bit_index_ == 8) {
        bit_index_ = 0;
        byte_index_++;
    }

    return true;
}

void report_series_set_error(char *error,
                             size_t error_len,
                             const char *message) {
    if (!error || error_len == 0) return;

    snprintf(error, error_len, "%s", message ? message : "");
}

int64_t report_series_zigzag_decode(uint64_t value) {
    return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

bool report_series_power_of_two_positive(int32_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

bool report_series_read_rice(ReportSeriesBitReader &bits,
                             int32_t modulus,
                             uint64_t &out) {
    if (!report_series_power_of_two_positive(modulus)) return false;

    uint64_t q = 0;
    uint8_t bit = 0;
    while (true) {
        if (!bits.read_bit(bit)) return false;
        if (bit == 0) break;
        if (q == UINT64_MAX) return false;

        q++;
    }

    uint64_t rem = 0;
    int32_t width = 0;
    for (int32_t m = modulus; m > 1; m >>= 1) width++;

    for (int32_t i = 0; i < width; ++i) {
        if (!bits.read_bit(bit)) return false;
        rem = (rem << 1) | bit;
    }

    if (q > (UINT64_MAX - rem) / static_cast<uint64_t>(modulus)) {
        return false;
    }

    out = q * static_cast<uint64_t>(modulus) + rem;
    return true;
}

int16_t report_series_read_le_i16(const uint8_t *data) {
    const uint16_t value = static_cast<uint16_t>(data[0]) |
                           (static_cast<uint16_t>(data[1]) << 8);
    return static_cast<int16_t>(value);
}

void report_series_put_le32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

int32_t report_series_milli_from_scaled(int64_t raw, double scale) {
    const double value = static_cast<double>(raw) * scale * 1000.0;
    if (value >= 0.0) {
        return static_cast<int32_t>(value + 0.5);
    }

    return static_cast<int32_t>(value - 0.5);
}

bool report_series_append_scaled_value_le(ReportSpoolBuffer &values,
                                          int64_t raw,
                                          double scale) {
    size_t offset = 0;
    uint8_t *dst = values.append_uninitialized(4, offset);
    if (!dst) return false;

    report_series_put_le32(
        dst,
        static_cast<uint32_t>(report_series_milli_from_scaled(raw, scale)));

    return true;
}

}  // namespace aircannect
