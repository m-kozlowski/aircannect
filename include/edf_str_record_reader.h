#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

bool edf_str_record_read_digital(const uint8_t *record,
                                 size_t len,
                                 const char *label,
                                 size_t sample_index,
                                 int16_t &out);
bool edf_str_record_read_physical(const uint8_t *record,
                                  size_t len,
                                  const char *label,
                                  size_t sample_index,
                                  float &out);

}  // namespace aircannect
