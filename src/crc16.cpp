#include "crc16.h"

#include <array>
#include <utility>

namespace aircannect {
namespace {

constexpr uint16_t crc16_ccitt_false_table_value(size_t value) {
    uint16_t crc = static_cast<uint16_t>(value << 8);
    for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000)
                  ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                  : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

template <size_t... Index>
constexpr std::array<uint16_t, sizeof...(Index)>
make_crc16_ccitt_false_table(std::index_sequence<Index...>) {
    return {{crc16_ccitt_false_table_value(Index)...}};
}

constexpr auto CRC16_CCITT_FALSE_TABLE =
    make_crc16_ccitt_false_table(std::make_index_sequence<256>{});

}  // namespace

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len) {
    uint16_t crc = 0xffff;
    if (!data && len) return crc;

    for (size_t i = 0; i < len; ++i) {
        const uint8_t index =
            static_cast<uint8_t>((crc >> 8) ^ data[i]);
        crc = static_cast<uint16_t>(
            (crc << 8) ^ CRC16_CCITT_FALSE_TABLE[index]);
    }
    return crc;
}

}  // namespace aircannect
