#pragma once

#include <stddef.h>
#include <string>

#include "app_config.h"
#include "ota_manager.h"
#include "wifi_manager.h"

namespace aircannect {

class EdfRecorderManager;

struct AppConfigUpdateRuntime {
    WifiModeState wifi_mode = WifiModeState::Off;
    bool has_sta_config = false;
};

struct AppConfigUpdateResult {
    size_t accepted_fields = 0;
    size_t changed_fields = 0;
    bool persisted = true;
    bool hostname_changed = false;
    bool wifi_country_changed = false;
    bool softap_changed = false;
    bool edf_capture_changed = false;
    bool ota_config_dirty = false;
    bool wifi_reconnect_required = false;
};

bool apply_web_config_update(AppConfig &config,
                             const std::string &body,
                             const AppConfigUpdateRuntime &runtime,
                             AppConfigUpdateResult &result);

void apply_config_runtime_effects(const AppConfigUpdateResult &result,
                                  AppConfig &config,
                                  WifiManager &wifi_manager,
                                  EdfRecorderManager &edf_recorder_manager,
                                  OtaManager &ota_manager);

}  // namespace aircannect
