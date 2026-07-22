#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr uint16_t PLX_SFLOAT_NAN = 0x07ff;
static constexpr uint16_t PLX_SFLOAT_NRES = 0x0800;
static constexpr uint16_t PLX_SFLOAT_POS_INF = 0x07fe;
static constexpr uint16_t PLX_SFLOAT_NEG_INF = 0x0802;
static constexpr uint16_t PLX_SFLOAT_RESERVED = 0x0801;

uint16_t encode_sfloat_int_value(int16_t value);
int16_t decode_sfloat_int_value(uint16_t raw, bool &valid);
uint8_t crc8_ccitt(const uint8_t *data,
                   size_t len,
                   uint8_t crc = 0x00);

}  // namespace aircannect
