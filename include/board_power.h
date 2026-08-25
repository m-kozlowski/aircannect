#pragma once

#ifndef AC_POWER_HOLD_GPIO
#define AC_POWER_HOLD_GPIO -1
#endif

#ifndef AC_POWER_HOLD_ACTIVE_HIGH
#define AC_POWER_HOLD_ACTIVE_HIGH 1
#endif

namespace aircannect {

void board_power_begin();

}  // namespace aircannect
