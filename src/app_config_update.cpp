#include "app_config_update.h"

#include <ArduinoJson.h>

#include "app_config_registry.h"
#include "debug_log.h"
#include "edf_recorder_manager.h"

namespace aircannect {
namespace {

bool json_value_to_config_text(JsonVariantConst value,
                               const AppConfigFieldDescriptor &field,
                               String &out) {
    out = "";
    switch (field.type) {
        case AppConfigFieldType::Bool:
            if (value.is<bool>()) {
                out = value.as<bool>() ? "1" : "0";
                return true;
            }
            if (value.is<const char *>()) {
                out = value.as<const char *>();
                return true;
            }
            return false;

        case AppConfigFieldType::UInt16:
            if (value.is<int>()) {
                const int parsed = value.as<int>();
                if (parsed < 0 || parsed > 65535) return false;
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", parsed);
                out = buf;
                return true;
            }
            if (value.is<const char *>()) {
                out = value.as<const char *>();
                return true;
            }
            return false;

        case AppConfigFieldType::String:
        case AppConfigFieldType::Secret:
        case AppConfigFieldType::Enum:
        case AppConfigFieldType::LogLevel:
            if (!value.is<const char *>()) return false;
            out = value.as<const char *>();
            return true;
    }
    return false;
}

void note_dirty(uint32_t dirty,
                const AppConfig &config,
                const AppConfigUpdateRuntime &runtime,
                AppConfigUpdateResult &result) {
    if (dirty & AC_CONFIG_DIRTY_HOSTNAME) {
        result.hostname_changed = true;
        result.ota_config_dirty = true;
    }
    if (dirty & AC_CONFIG_DIRTY_WIFI_COUNTRY) {
        result.wifi_country_changed = true;
        result.wifi_reconnect_required = true;
    }
    if (dirty & AC_CONFIG_DIRTY_SOFTAP) {
        result.softap_changed = true;
        result.wifi_reconnect_required =
            result.wifi_reconnect_required ||
            (runtime.wifi_mode == WifiModeState::SoftAp &&
             config.data().softap_mode == SoftApMode::Auto &&
             runtime.has_sta_config);
    }
    if (dirty & AC_CONFIG_DIRTY_EDF_CAPTURE) {
        result.edf_capture_changed = true;
    }
    if (dirty & AC_CONFIG_DIRTY_OTA_PASSWORD) {
        result.ota_config_dirty = true;
    }
    if (dirty & (AC_CONFIG_DIRTY_LOG_LEVELS |
                 AC_CONFIG_DIRTY_SYSLOG |
                 AC_CONFIG_DIRTY_FILE_LOG)) {
        result.log_config_changed = true;
    }
}

}  // namespace

bool apply_web_config_update(AppConfig &config,
                             const std::string &body,
                             const AppConfigUpdateRuntime &runtime,
                             AppConfigUpdateResult &result) {
    result = AppConfigUpdateResult();

    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) return false;

    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) return false;

    config.begin_update();
    for (JsonPairConst pair : root) {
        const char *key = pair.key().c_str();
        const AppConfigFieldDescriptor *field = app_config_find_field(key);
        if (!field) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=unknown\n",
                      key ? key : "<null>");
            continue;
        }

        String value;
        if (!json_value_to_config_text(pair.value(), *field, value)) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=bad_type\n",
                      field->key);
            continue;
        }

        AppConfigFieldSetResult field_result;
        if (!app_config_field_set_in_update(config, *field, value, true,
                                            field_result)) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=invalid_value\n",
                      field->key);
            continue;
        }

        if (!field_result.accepted) continue;
        result.accepted_fields++;
        if (!field_result.changed) continue;
        result.changed_fields++;
        note_dirty(field_result.dirty, config, runtime, result);
    }

    result.persisted = config.commit_update();
    return true;
}

void apply_config_runtime_effects(const AppConfigUpdateResult &result,
                                  AppConfig &config,
                                  WifiManager &wifi_manager,
                                  EdfRecorderManager &edf_recorder_manager,
                                  OtaManager &ota_manager) {
    if (result.hostname_changed) {
        wifi_manager.set_hostname(config.data().hostname);
    }
    if (result.wifi_country_changed) {
        wifi_manager.set_country_code(config.data().wifi_country);
    }
    if (result.softap_changed) {
        wifi_manager.set_softap_mode(config.data().softap_mode);
        wifi_manager.apply_softap_mode();
    }
    if (result.ota_config_dirty) {
        ota_manager.mark_config_dirty();
    }
    if (result.log_config_changed) {
        config.apply_log_config();
    }
    if (result.edf_capture_changed) {
        edf_recorder_manager.set_enabled(config.data().edf_capture_enabled);
    }
    if (result.wifi_reconnect_required) {
        wifi_manager.reconnect();
    }
}

}  // namespace aircannect
