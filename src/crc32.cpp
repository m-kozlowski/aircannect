#include "crc32.h"

namespace aircannect {

uint32_t crc32_ieee_initial_state() {
    return 0xFFFFFFFFu;
}

uint32_t crc32_ieee_update_state(uint32_t crc,
                                 const uint8_t *data,
                                 size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
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
