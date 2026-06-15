#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

uint32_t crc32_ieee_initial_state();
uint32_t crc32_ieee_update_state(uint32_t crc,
                                 const uint8_t *data,
                                 size_t len);
uint32_t crc32_ieee_finish_state(uint32_t crc);
uint32_t crc32_ieee(const uint8_t *data, size_t len);

}  // namespace aircannect
