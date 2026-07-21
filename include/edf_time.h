#pragma once

#include <stdint.h>

#include "as11_clock.h"

namespace aircannect {

bool edf_parse_utc_ms(const char *text, int64_t &epoch_ms);
bool edf_parse_as11_utc_ms(const char *text,
                           const As11ClockTransform &transform,
                           int64_t &epoch_ms);
int64_t edf_floor_epoch_ms_to_second(int64_t epoch_ms);

}  // namespace aircannect
