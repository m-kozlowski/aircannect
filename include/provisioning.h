#pragma once

#include "app_config.h"
#include "wifi_manager.h"

namespace aircannect {

class StoragePathPort;
class StorageReadPort;

void apply_storage_provisioning(AppConfig &app_config,
                                WifiManager &wifi_manager,
                                StorageReadPort &read_port,
                                StoragePathPort &path_port);

}  // namespace aircannect
