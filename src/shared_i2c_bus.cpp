#include "shared_i2c_bus.h"

#include <Arduino.h>

#include "board_i2c.h"

namespace aircannect {

i2c_master_bus_handle_t board_shared_i2c_bus() {
    static i2c_master_bus_handle_t bus = nullptr;
    if (bus) return bus;
    if (AC_BOARD_I2C_SDA_GPIO < 0 || AC_BOARD_I2C_SCL_GPIO < 0) {
        return nullptr;
    }

    if (i2c_master_get_bus_handle(
            static_cast<i2c_port_num_t>(AC_BOARD_I2C_PORT), &bus) == ESP_OK) {
        return bus;
    }

    i2c_master_bus_config_t config = {};
    config.i2c_port = static_cast<i2c_port_num_t>(AC_BOARD_I2C_PORT);
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.sda_io_num =
        static_cast<gpio_num_t>(AC_BOARD_I2C_SDA_GPIO);
    config.scl_io_num =
        static_cast<gpio_num_t>(AC_BOARD_I2C_SCL_GPIO);
    config.glitch_ignore_cnt = 7;
    config.trans_queue_depth = 0;
    config.flags.enable_internal_pullup = AC_BOARD_I2C_PULLUPS != 0;
    config.flags.allow_pd = false;

    if (i2c_new_master_bus(&config, &bus) != ESP_OK) bus = nullptr;
    return bus;
}

}  // namespace aircannect
