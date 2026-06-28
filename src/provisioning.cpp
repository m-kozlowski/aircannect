#include "provisioning.h"

#include <stdlib.h>

#include "board.h"
#include "debug_log.h"
#include "softap_mode.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {

namespace {

struct ProvisionState {
    bool tcp_dirty = false;
    bool tcp_enabled = false;
    uint16_t tcp_port = 0;

    bool telnet_dirty = false;
    bool telnet_enabled = false;
    uint16_t telnet_port = 0;

    bool syslog_dirty = false;
    bool syslog_enabled = false;
    String syslog_host;
    uint16_t syslog_port = 0;

    bool oximetry_dirty = false;
    bool oximetry_enabled = false;
    uint16_t oximetry_udp_port = 0;
    OximetryAdvertiseMode oximetry_advertise_mode =
        OximetryAdvertiseMode::Auto;

    bool smb_dirty = false;
    String smb_endpoint;
    String smb_user;
    String smb_password;

    bool sleephq_dirty = false;
    String sleephq_client_id;
    String sleephq_client_secret;
    String sleephq_team_id;
    String sleephq_device_id;

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

bool parse_bool_value(const String &text, bool &value) {
    return parse_bool_yesno(text, value);
}

bool parse_port_value(const String &text, uint16_t &port) {
    return parse_port(text, port);
}

bool parse_log_key(const String &key, log_cat_t &cat) {
    cat = CAT_GENERAL;
    if (!key.startsWith("log") || key.length() <= 3) return false;
    String index_text = key.substring(3);
    for (size_t i = 0; i < index_text.length(); ++i) {
        if (!isDigit(index_text[i])) return false;
    }
    const int parsed = index_text.toInt();
    if (parsed < 0 || parsed >= CAT_COUNT) return false;
    cat = static_cast<log_cat_t>(parsed);
    return true;
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
                   ProvisionState &state,
                   const String &key,
                   const String &value,
                   bool &known) {
    known = true;
    bool bool_value = false;
    uint16_t port_value = 0;

    if (key == "host") {
        return app_config.set_hostname(value);
    }
    if (key == "tcp_en") {
        if (!parse_bool_value(value, bool_value)) return false;
        state.tcp_enabled = bool_value;
        state.tcp_dirty = true;
        return true;
    }
    if (key == "tcp_port") {
        if (!parse_port_value(value, port_value)) return false;
        state.tcp_port = port_value;
        state.tcp_dirty = true;
        return true;
    }
    if (key == "softap_mode") {
        SoftApMode mode;
        if (!parse_softap_mode(value, mode)) return false;
        return app_config.set_softap_mode(mode);
    }
    if (key == "wifi_ctry") {
        return app_config.set_wifi_country(value);
    }
    if (key == "tz") {
        return app_config.set_timezone(value);
    }
    if (key == "resmed_time") {
        if (!parse_bool_value(value, bool_value)) return false;
        return app_config.set_resmed_time_sync(bool_value);
    }
    if (key == "oxi_en") {
        if (!parse_bool_value(value, bool_value)) return false;
        state.oximetry_enabled = bool_value;
        state.oximetry_dirty = true;
        return true;
    }
    if (key == "oxi_udp") {
        if (!parse_port_value(value, port_value)) return false;
        state.oximetry_udp_port = port_value;
        state.oximetry_dirty = true;
        return true;
    }
    if (key == "oxi_adv") {
        OximetryAdvertiseMode mode;
        if (!parse_oximetry_advertise_mode(value, mode)) return false;
        state.oximetry_advertise_mode = mode;
        state.oximetry_dirty = true;
        return true;
    }
    if (key == "edf_cap") {
        if (!parse_bool_value(value, bool_value)) return false;
        return app_config.set_edf_capture_enabled(bool_value);
    }
    if (key == "smb_ep") {
        state.smb_endpoint = value;
        state.smb_dirty = true;
        return true;
    }
    if (key == "smb_user") {
        state.smb_user = value;
        state.smb_dirty = true;
        return true;
    }
    if (key == "smb_pass") {
        state.smb_password = value;
        state.smb_dirty = true;
        return true;
    }
    if (key == "shq_id") {
        state.sleephq_client_id = value;
        state.sleephq_dirty = true;
        return true;
    }
    if (key == "shq_secret") {
        state.sleephq_client_secret = value;
        state.sleephq_dirty = true;
        return true;
    }
    if (key == "shq_team") {
        state.sleephq_team_id = value;
        state.sleephq_dirty = true;
        return true;
    }
    if (key == "shq_device") {
        state.sleephq_device_id = value;
        state.sleephq_dirty = true;
        return true;
    }
    if (key == "http_user") {
        return app_config.set_http_auth(value,
                                        app_config.data().http_password);
    }
    if (key == "http_pass") {
        return app_config.set_http_auth(app_config.data().http_user, value);
    }
    if (key == "auth_wl") {
        return app_config.set_auth_whitelist(value);
    }
    if (key == "telnet_en") {
        if (!parse_bool_value(value, bool_value)) return false;
        state.telnet_enabled = bool_value;
        state.telnet_dirty = true;
        return true;
    }
    if (key == "telnet_port") {
        if (!parse_port_value(value, port_value)) return false;
        state.telnet_port = port_value;
        state.telnet_dirty = true;
        return true;
    }
    if (key == "ota_pass") {
        return app_config.set_ota_password(value);
    }
    if (key == "syslog_en") {
        if (!parse_bool_value(value, bool_value)) return false;
        state.syslog_enabled = bool_value;
        state.syslog_dirty = true;
        return true;
    }
    if (key == "syslog_host") {
        state.syslog_host = value;
        state.syslog_dirty = true;
        return true;
    }
    if (key == "syslog_port") {
        if (!parse_port_value(value, port_value)) return false;
        state.syslog_port = port_value;
        state.syslog_dirty = true;
        return true;
    }
    if (key == "file_log_en") {
        if (!parse_bool_value(value, bool_value)) return false;
        return app_config.set_file_log(bool_value);
    }
    log_cat_t log_cat = CAT_GENERAL;
    if (parse_log_key(key, log_cat)) {
        log_level_t level = LOG_INFO;
        if (!Log::parse_level(value, level)) return false;
        return app_config.set_log_level(log_cat, level);
    }

    known = false;
    return true;
}

bool commit_grouped_config(AppConfig &app_config, ProvisionState &state) {
    bool ok = true;
    if (state.tcp_dirty) {
        ok = app_config.set_tcp_bridge(state.tcp_enabled,
                                       state.tcp_port) &&
             ok;
    }
    if (state.telnet_dirty) {
        ok = app_config.set_telnet_console(state.telnet_enabled,
                                           state.telnet_port) &&
             ok;
    }
    if (state.syslog_dirty) {
        ok = app_config.set_syslog(state.syslog_enabled,
                                   state.syslog_host,
                                   state.syslog_port) &&
             ok;
    }
    if (state.oximetry_dirty) {
        ok = app_config.set_oximetry_enabled(
                 state.oximetry_enabled) &&
             ok;
        ok = app_config.set_oximetry_udp_port(
                 state.oximetry_udp_port) &&
             ok;
        ok = app_config.set_oximetry_advertise_mode(
                 state.oximetry_advertise_mode) &&
             ok;
    }
    if (state.smb_dirty) {
        ok = app_config.set_smb_credentials(state.smb_endpoint,
                                            state.smb_user,
                                            state.smb_password) &&
             ok;
    }
    if (state.sleephq_dirty) {
        ok = app_config.set_sleephq_credentials(
                 state.sleephq_client_id,
                 state.sleephq_client_secret,
                 state.sleephq_team_id,
                 state.sleephq_device_id) &&
             ok;
    }
    return ok;
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
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[PROVISION] failed to open %s\n",
                  AC_PROVISION_CONFIG_PATH);
        return;
    }

    ProvisionState state;
    const AppConfigData &cfg = app_config.data();
    state.tcp_enabled = cfg.tcp_bridge_enabled;
    state.tcp_port = cfg.tcp_bridge_port;
    state.telnet_enabled = cfg.telnet_console_enabled;
    state.telnet_port = cfg.telnet_console_port;
    state.syslog_enabled = cfg.syslog_enabled;
    state.syslog_host = cfg.syslog_host;
    state.syslog_port = cfg.syslog_port;
    state.oximetry_enabled = cfg.oximetry_enabled;
    state.oximetry_udp_port = cfg.oximetry_udp_port;
    state.oximetry_advertise_mode = cfg.oximetry_advertise_mode;
    state.smb_endpoint = cfg.smb_endpoint;
    state.smb_user = cfg.smb_user;
    state.smb_password = cfg.smb_password;
    state.sleephq_client_id = cfg.sleephq_client_id;
    state.sleephq_client_secret = cfg.sleephq_client_secret;
    state.sleephq_team_id = cfg.sleephq_team_id;
    state.sleephq_device_id = cfg.sleephq_device_id;

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
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[PROVISION] invalid line %u\n",
                      static_cast<unsigned>(lines));
            continue;
        }

        String key = parse_key(line.substring(0, eq));
        String value = line.substring(eq + 1);

        bool known = false;
        bool ok = apply_wifi_key(state, key, value, known);
        if (!known) {
            ok = apply_app_key(app_config, state, key, value, known);
        }
        if (known && ok) {
            applied++;
        } else {
            rejected++;
            Log::logf(CAT_GENERAL, LOG_WARN,
                      "[PROVISION] rejected key=%s\n", key.c_str());
        }
    }
    file.close();

    bool ok = commit_grouped_config(app_config, state);
    ok = app_config.commit_update() && ok;
    ok = commit_wifi(wifi_manager, state) && ok;

    Storage::remove(AC_PROVISION_OK_PATH);
    const bool renamed = Storage::rename(AC_PROVISION_CONFIG_PATH,
                                         AC_PROVISION_OK_PATH);
    Log::logf(CAT_GENERAL, ok ? LOG_INFO : LOG_WARN,
              "[PROVISION] config.txt applied=%u rejected=%u status=%s\n",
              static_cast<unsigned>(applied),
              static_cast<unsigned>(rejected),
              ok ? "ok" : "partial");
    if (!renamed) {
        Log::logf(CAT_GENERAL, LOG_WARN,
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
    Log::logf(CAT_GENERAL, LOG_INFO, "[PROVISION] found %s\n",
              AC_PROVISION_CONFIG_PATH);
    consume_config_file(app_config, wifi_manager);
}

}  // namespace aircannect
