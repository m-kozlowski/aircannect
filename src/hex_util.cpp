#include "hex_util.h"

#include <string.h>

#include "checked_size.h"

namespace aircannect {
namespace {

const char *digits_for(HexCase letter_case) {
    return letter_case == HexCase::Upper
        ? "0123456789ABCDEF"
        : "0123456789abcdef";
}

}  // namespace

int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

char hex_digit(uint8_t value, HexCase letter_case) {
    return digits_for(letter_case)[value & 0x0f];
}

bool hex_text_valid(const char *value, size_t length) {
    if (!value) return false;
    for (size_t i = 0; i < length; ++i) {
        if (hex_nibble(value[i]) < 0) return false;
    }
    return true;
}

bool hex_text_normalize(const char *value,
                        size_t length,
                        char *out,
                        size_t out_size,
                        HexCase letter_case) {
    if (!value || !out || out_size <= length ||
        !hex_text_valid(value, length)) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        out[i] = hex_digit(static_cast<uint8_t>(hex_nibble(value[i])),
                           letter_case);
    }
    out[length] = '\0';
    return true;
}

bool hex_encode(const uint8_t *bytes,
                size_t length,
                char *out,
                size_t out_size,
                HexCase letter_case) {
    size_t encoded_length = 0;
    if (!bytes || !out ||
        !CheckedSize::multiply(length, 2, encoded_length) ||
        out_size <= encoded_length) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        out[i * 2] = hex_digit(bytes[i] >> 4, letter_case);
        out[i * 2 + 1] = hex_digit(bytes[i], letter_case);
    }
    out[encoded_length] = '\0';
    return true;
}

bool sha256_text_valid(const char *value) {
    return value && strlen(value) == 64 && hex_text_valid(value, 64);
}

}  // namespace aircannect
