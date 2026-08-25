#include "board_power.h"

#include <Arduino.h>

namespace aircannect {

void board_power_begin() {
#if AC_POWER_HOLD_GPIO >= 0
    pinMode(AC_POWER_HOLD_GPIO, OUTPUT);
    digitalWrite(AC_POWER_HOLD_GPIO,
                 AC_POWER_HOLD_ACTIVE_HIGH ? HIGH : LOW);
#endif
}

}  // namespace aircannect
