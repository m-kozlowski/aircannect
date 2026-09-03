#include "crc32.h"

namespace aircannect {
namespace {

constexpr uint32_t CRC32_NIBBLE_TABLE[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
};

}  // namespace

uint32_t crc32_ieee_initial_state() {
    return 0xFFFFFFFFu;
}

uint32_t crc32_ieee_update_state(uint32_t crc,
                                 const uint8_t *data,
                                 size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ CRC32_NIBBLE_TABLE[crc & 0x0fu];
        crc = (crc >> 4) ^ CRC32_NIBBLE_TABLE[crc & 0x0fu];
    }
    return crc;
}

uint32_t crc32_ieee_finish_state(uint32_t crc) {
    return ~crc;
}

uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    return crc32_ieee_finish_state(
        crc32_ieee_update_state(crc32_ieee_initial_state(), data, len));
}

}  // namespace aircannect
