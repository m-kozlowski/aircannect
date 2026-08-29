#include "board_power.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace aircannect {

void board_power_begin() {
#if AC_POWER_HOLD_GPIO >= 0
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
        gpio_deep_sleep_hold_dis();
        gpio_hold_dis(static_cast<gpio_num_t>(AC_POWER_HOLD_GPIO));
    }

    pinMode(AC_POWER_HOLD_GPIO, OUTPUT);
    digitalWrite(AC_POWER_HOLD_GPIO,
                 AC_POWER_HOLD_ACTIVE_HIGH ? HIGH : LOW);
#endif
}

bool board_power_off_supported() {
    return AC_POWER_HOLD_GPIO >= 0 || AC_POWER_WAKE_GPIO >= 0;
}

bool board_power_off() {
#if AC_POWER_WAKE_GPIO >= 0
    pinMode(AC_POWER_WAKE_GPIO,
            AC_POWER_WAKE_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);

    const int wake_level = AC_POWER_WAKE_ACTIVE_LOW ? LOW : HIGH;
    while (digitalRead(AC_POWER_WAKE_GPIO) == wake_level) {
        delay(10);
    }

    if (esp_sleep_enable_ext0_wakeup(
            static_cast<gpio_num_t>(AC_POWER_WAKE_GPIO),
            wake_level) != ESP_OK) {
        return false;
    }
#endif

#if AC_POWER_HOLD_GPIO >= 0
    digitalWrite(AC_POWER_HOLD_GPIO,
                 AC_POWER_HOLD_ACTIVE_HIGH ? LOW : HIGH);

#if AC_POWER_WAKE_GPIO >= 0
    if (gpio_hold_en(static_cast<gpio_num_t>(AC_POWER_HOLD_GPIO)) == ESP_OK) {
        gpio_deep_sleep_hold_en();
    }
#endif
#endif

#if AC_POWER_WAKE_GPIO >= 0
    Serial.flush();
    esp_deep_sleep_start();
#endif

#if AC_POWER_HOLD_GPIO >= 0
    return true;
#else
    return false;
#endif
}

}  // namespace aircannect
