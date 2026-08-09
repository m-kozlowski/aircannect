#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

}  // namespace aircannect
