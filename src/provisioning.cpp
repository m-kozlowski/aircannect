#include "provisioning.h"

#include "app_config_registry.h"
#include "board.h"
#include "debug_log.h"
#include "storage_manager.h"

namespace aircannect {

namespace {

struct ProvisionState {
    bool wifi_profile_touched = false;
    WifiProfile wifi[AC_WIFI_PROFILE_MAX];
    bool wifi_touched[AC_WIFI_PROFILE_MAX] = {};
};

String parse_key(String key) {
    key.trim();
    return key;
}

void strip_line_ending(String &line) {
    while (line.length() > 0 && line[line.length() - 1] == '\r') {
        line.remove(line.length() - 1);
    }
}

bool parse_wifi_key(const String &key, size_t &index, String &field) {
    index = 0;
    field = "";
    if (key.startsWith("ssid_")) field = "ssid";
    else if (key.startsWith("pass_")) field = "pass";
    else return false;

    String index_text = key.substring(5);
    if (!index_text.length()) return false;
    for (size_t i = 0; i < index_text.length(); ++i) {
        if (!isDigit(index_text[i])) return false;
    }
    const int parsed = index_text.toInt();
    if (parsed < 0 || parsed >= static_cast<int>(AC_WIFI_PROFILE_MAX)) {
        return false;
    }
    index = static_cast<size_t>(parsed);
    return true;
}

bool apply_wifi_key(ProvisionState &state,
                    const String &key,
                    const String &value,
                    bool &known) {
    size_t index = 0;
    String field;
    known = parse_wifi_key(key, index, field);
    if (!known) return true;

    if (index >= AC_WIFI_PROFILE_MAX) return false;
    if (field == "ssid") {
        state.wifi_profile_touched = true;
        state.wifi_touched[index] = true;
        state.wifi[index].ssid = value;
        return true;
    }
    if (field == "pass") {
        state.wifi[index].password = value;
        return true;
    }
    return false;
}

bool apply_app_key(AppConfig &app_config,
                   const String &key,
                   const String &value,
                   bool &known) {
    const AppConfigFieldDescriptor *field =
        app_config_find_field(key.c_str());
    known = field != nullptr;
    if (!field) return true;
    if ((field->flags & AC_CONFIG_FIELD_PROVISIONABLE) == 0) return false;

    AppConfigFieldSetResult result;
    return app_config_field_set_in_update(app_config, *field, value,
                                          false, result);
}

bool commit_wifi(WifiManager &wifi_manager, ProvisionState &state) {
    if (!state.wifi_profile_touched) return true;

    WifiProfile profiles[AC_WIFI_PROFILE_MAX];
    size_t count = 0;
    for (size_t i = 0; i < AC_WIFI_PROFILE_MAX; ++i) {
        if (!state.wifi_touched[i]) continue;
        profiles[count++] = state.wifi[i];
    }
    return wifi_manager.replace_profiles(profiles, count, false);
}

void consume_config_file(AppConfig &app_config, WifiManager &wifi_manager) {
    File file = Storage::open(AC_PROVISION_CONFIG_PATH, "r");
    if (!file) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] failed to open %s\n",
                  AC_PROVISION_CONFIG_PATH);
        return;
    }

    ProvisionState state;

    size_t lines = 0;
    size_t applied = 0;
    size_t rejected = 0;
    app_config.begin_update();
    while (file.available()) {
        String line = file.readStringUntil('\n');
        strip_line_ending(line);
        if (line.length() > AC_PROVISION_LINE_MAX) {
            rejected++;
            continue;
        }
        String line_for_control = line;
        line_for_control.trim();
        if (!line_for_control.length() ||
            line_for_control[0] == '#' ||
            line_for_control[0] == ';') {
            continue;
        }
        lines++;

        const int eq = line.indexOf('=');
        if (eq <= 0) {
            rejected++;
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "[PROVISION] invalid line %u\n",
                      static_cast<unsigned>(lines));
            continue;
        }

        String key = parse_key(line.substring(0, eq));
        String value = line.substring(eq + 1);

        bool known = false;
        bool ok = apply_wifi_key(state, key, value, known);
        if (!known) {
            ok = apply_app_key(app_config, key, value, known);
        }
        if (known && ok) {
            applied++;
        } else {
            rejected++;
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "[PROVISION] rejected key=%s\n", key.c_str());
        }
    }
    file.close();

    bool ok = app_config.commit_update();
    ok = commit_wifi(wifi_manager, state) && ok;

    Storage::remove(AC_PROVISION_OK_PATH);
    const bool renamed = Storage::rename(AC_PROVISION_CONFIG_PATH,
                                         AC_PROVISION_OK_PATH);
    Log::logf(CAT_CONFIG, ok ? LOG_INFO : LOG_WARN,
              "[PROVISION] config.txt applied=%u rejected=%u status=%s\n",
              static_cast<unsigned>(applied),
              static_cast<unsigned>(rejected),
              ok ? "ok" : "partial");
    if (!renamed) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] failed to rename %s to %s\n",
                  AC_PROVISION_CONFIG_PATH, AC_PROVISION_OK_PATH);
    }
}

}  // namespace

void apply_storage_provisioning(AppConfig &app_config,
                                WifiManager &wifi_manager) {
    if (!Storage::mounted() || !Storage::exists(AC_PROVISION_CONFIG_PATH)) {
        return;
    }
    Log::logf(CAT_CONFIG, LOG_INFO, "[PROVISION] found %s\n",
              AC_PROVISION_CONFIG_PATH);
    consume_config_file(app_config, wifi_manager);
}

}  // namespace aircannect
