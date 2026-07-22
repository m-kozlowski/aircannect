#pragma once

#include "config_service.h"
#include "wifi_manager.h"

namespace aircannect {

class StoragePathPort;
class StorageReadPort;

void apply_storage_provisioning(ConfigService &config_service,
                                WifiManager &wifi_manager,
                                StorageReadPort &read_port,
                                StoragePathPort &path_port);

}  // namespace aircannect
