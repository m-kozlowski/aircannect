#pragma once

#include <driver/i2c_master.h>

namespace aircannect {

i2c_master_bus_handle_t board_shared_i2c_bus();

}  // namespace aircannect
