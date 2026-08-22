#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class HexCase : uint8_t {
    Lower,
    Upper,
};

int hex_nibble(char value);
char hex_digit(uint8_t value, HexCase letter_case);
bool hex_text_valid(const char *value, size_t length);
bool hex_text_normalize(const char *value,
                        size_t length,
                        char *out,
                        size_t out_size,
                        HexCase letter_case);
bool hex_encode(const uint8_t *bytes,
                size_t length,
                char *out,
                size_t out_size,
                HexCase letter_case);
bool hex_decode(const char *text,
                size_t text_length,
                uint8_t *out,
                size_t out_size,
                size_t &decoded_length);
bool sha256_text_valid(const char *value);

}  // namespace aircannect
