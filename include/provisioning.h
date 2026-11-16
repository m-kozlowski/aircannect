#pragma once

#include "config_service.h"
#include "wifi_manager.h"

namespace aircannect {

class BleSensorSource;
class StoragePathPort;
class StorageReadPort;

// Applies /config.txt through the same restore as WebUI backups.
void apply_storage_provisioning(ConfigService &config_service,
                                WifiManager &wifi_manager,
                                BleSensorSource *sensors,
                                StorageReadPort &read_port,
                                StoragePathPort &path_port);

}  // namespace aircannect
