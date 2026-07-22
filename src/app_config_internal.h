#pragma once

#include <Arduino.h>

#include "app_config.h"
#include "app_config_registry.h"

namespace aircannect {

struct AppConfigStoreFieldResult {
    bool accepted = false;
    bool changed = false;
    uint32_t dirty = 0;
};

class AppConfigFieldWriter {
public:
    static bool set_in_update(AppConfig &config,
                              const AppConfigFieldDescriptor &field,
                              const String &value,
                              bool preserve_secret_sentinel,
                              AppConfigStoreFieldResult &result);

private:
    static bool set_value(AppConfig &config,
                          const AppConfigFieldDescriptor &field,
                          const String &value,
                          bool preserve_secret_sentinel);
};

}  // namespace aircannect
