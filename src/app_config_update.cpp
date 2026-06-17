#include "app_config_update.h"

#include <ArduinoJson.h>

#include "edf_recorder_manager.h"

namespace aircannect {
namespace {

bool json_get_string(JsonDocument &doc, const char *key, String &out) {
    if (!doc[key].is<const char *>()) return false;
    out = doc[key].as<const char *>();
    return true;
}

bool note_bool_change(bool before, bool after, bool accepted,
                      AppConfigUpdateResult &result) {
    if (!accepted) return false;
    result.accepted_fields++;
    if (before == after) return false;
    result.changed_fields++;
    return true;
}

bool note_uint16_change(uint16_t before, uint16_t after, bool accepted,
                        AppConfigUpdateResult &result) {
    if (!accepted) return false;
    result.accepted_fields++;
    if (before == after) return false;
    result.changed_fields++;
    return true;
}

bool note_string_change(const String &before, const String &after,
                        bool accepted, AppConfigUpdateResult &result) {
    if (!accepted) return false;
    result.accepted_fields++;
    if (before == after) return false;
    result.changed_fields++;
    return true;
}

bool note_softap_change(SoftApMode before, SoftApMode after, bool accepted,
                        AppConfigUpdateResult &result) {
    if (!accepted) return false;
    result.accepted_fields++;
    if (before == after) return false;
    result.changed_fields++;
    return true;
}

bool note_oximetry_advertise_change(OximetryAdvertiseMode before,
                                    OximetryAdvertiseMode after,
                                    bool accepted,
                                    AppConfigUpdateResult &result) {
    if (!accepted) return false;
    result.accepted_fields++;
    if (before == after) return false;
    result.changed_fields++;
    return true;
}

bool valid_port_from_json(JsonDocument &doc, const char *key,
                          uint16_t &out) {
    if (!doc[key].is<int>()) return false;
    const int parsed = doc[key].as<int>();
    if (parsed <= 0 || parsed > 65535) return false;
    out = static_cast<uint16_t>(parsed);
    return true;
}

}  // namespace

bool apply_web_config_update(AppConfig &config,
                             const std::string &body,
                             const AppConfigUpdateRuntime &runtime,
                             AppConfigUpdateResult &result) {
    result = AppConfigUpdateResult();

    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) return false;

    String s;
    config.begin_update();

    if (json_get_string(doc, "hostname", s)) {
        const String before = config.data().hostname;
        const bool accepted = config.set_hostname(s);
        if (note_string_change(before, config.data().hostname,
                               accepted, result)) {
            result.hostname_changed = true;
            result.ota_config_dirty = true;
        }
    }

    if (json_get_string(doc, "wifi_country", s)) {
        const String before = config.data().wifi_country;
        const bool accepted = config.set_wifi_country(s);
        if (note_string_change(before, config.data().wifi_country,
                               accepted, result)) {
            result.wifi_country_changed = true;
            result.wifi_reconnect_required = true;
        }
    }

    if (json_get_string(doc, "timezone", s)) {
        const String before = config.data().timezone;
        const bool accepted = config.set_timezone(s);
        note_string_change(before, config.data().timezone, accepted, result);
    }

    if (doc["resmed_time_sync_enabled"].is<bool>()) {
        const bool before = config.data().resmed_time_sync_enabled;
        const bool accepted = config.set_resmed_time_sync(
            doc["resmed_time_sync_enabled"].as<bool>());
        note_bool_change(before, config.data().resmed_time_sync_enabled,
                         accepted, result);
    }

    if (doc["oximetry_enabled"].is<bool>()) {
        const bool before = config.data().oximetry_enabled;
        const bool accepted = config.set_oximetry_enabled(
            doc["oximetry_enabled"].as<bool>());
        note_bool_change(before, config.data().oximetry_enabled,
                         accepted, result);
    }

    if (doc["edf_capture_enabled"].is<bool>()) {
        const bool before = config.data().edf_capture_enabled;
        const bool accepted = config.set_edf_capture_enabled(
            doc["edf_capture_enabled"].as<bool>());
        if (note_bool_change(before, config.data().edf_capture_enabled,
                             accepted, result)) {
            result.edf_capture_changed = true;
        }
    }

    if (doc["smb_endpoint"].is<const char *>() ||
        doc["smb_user"].is<const char *>() ||
        doc["smb_password"].is<const char *>()) {
        String endpoint = config.data().smb_endpoint;
        String user = config.data().smb_user;
        String password = config.data().smb_password;
        if (doc["smb_endpoint"].is<const char *>()) {
            endpoint = doc["smb_endpoint"].as<const char *>();
        }
        if (doc["smb_user"].is<const char *>()) {
            user = doc["smb_user"].as<const char *>();
        }
        if (doc["smb_password"].is<const char *>()) {
            password = doc["smb_password"].as<const char *>();
            if (password == "********" &&
                config.data().smb_password.length() > 0) {
                password = config.data().smb_password;
            }
        }
        const String before_endpoint = config.data().smb_endpoint;
        const String before_user = config.data().smb_user;
        const String before_password = config.data().smb_password;
        const bool accepted =
            config.set_smb_credentials(endpoint, user, password);
        if (accepted) {
            result.accepted_fields++;
            if (before_endpoint != config.data().smb_endpoint ||
                before_user != config.data().smb_user ||
                before_password != config.data().smb_password) {
                result.changed_fields++;
            }
        }
    }

    uint16_t parsed_port = 0;
    if (valid_port_from_json(doc, "oximetry_udp_port", parsed_port)) {
        const uint16_t before = config.data().oximetry_udp_port;
        const bool accepted = config.set_oximetry_udp_port(parsed_port);
        note_uint16_change(before, config.data().oximetry_udp_port,
                           accepted, result);
    }

    if (json_get_string(doc, "oximetry_advertise_mode", s)) {
        OximetryAdvertiseMode mode;
        if (parse_oximetry_advertise_mode(s, mode)) {
            const OximetryAdvertiseMode before =
                config.data().oximetry_advertise_mode;
            const bool accepted = config.set_oximetry_advertise_mode(mode);
            note_oximetry_advertise_change(
                before, config.data().oximetry_advertise_mode,
                accepted, result);
        }
    }

    if (json_get_string(doc, "softap_mode", s)) {
        SoftApMode softap_mode;
        if (parse_softap_mode(s, softap_mode)) {
            const SoftApMode before = config.data().softap_mode;
            const bool accepted = config.set_softap_mode(softap_mode);
            if (note_softap_change(before, config.data().softap_mode,
                                   accepted, result)) {
                result.softap_changed = true;
                result.wifi_reconnect_required =
                    result.wifi_reconnect_required ||
                    (runtime.wifi_mode == WifiModeState::SoftAp &&
                     config.data().softap_mode == SoftApMode::Auto &&
                     runtime.has_sta_config);
            }
        }
    }

    const bool http_auth_present = doc["http_auth_required"].is<bool>();
    const bool disable_http_auth =
        http_auth_present && !doc["http_auth_required"].as<bool>();
    const bool http_user_present = doc["http_user"].is<const char *>();
    const bool http_password_present =
        doc["http_password"].is<const char *>();
    if (disable_http_auth || http_user_present || http_password_present) {
        String user = config.data().http_user;
        String password = config.data().http_password;
        if (disable_http_auth) {
            user = "";
            password = "";
        } else if (http_user_present) {
            user = doc["http_user"].as<const char *>();
        }
        if (!disable_http_auth && http_password_present) {
            password = doc["http_password"].as<const char *>();
        }
        const String before_user = config.data().http_user;
        const String before_password = config.data().http_password;
        const bool accepted = config.set_http_auth(user, password);
        if (accepted) {
            result.accepted_fields++;
            if (before_user != config.data().http_user ||
                before_password != config.data().http_password) {
                result.changed_fields++;
            }
        }
    }

    if (json_get_string(doc, "auth_whitelist", s)) {
        const String before = config.data().auth_whitelist;
        const bool accepted = config.set_auth_whitelist(s);
        note_string_change(before, config.data().auth_whitelist,
                           accepted, result);
    }

    if (json_get_string(doc, "ota_password", s)) {
        const String before = config.data().ota_password;
        const bool accepted = config.set_ota_password(s);
        if (note_string_change(before, config.data().ota_password,
                               accepted, result)) {
            result.ota_config_dirty = true;
        }
    }

    if (doc["tcp_enabled"].is<bool>() ||
        valid_port_from_json(doc, "tcp_port", parsed_port)) {
        bool enabled = config.data().tcp_bridge_enabled;
        uint16_t port = config.data().tcp_bridge_port;
        if (doc["tcp_enabled"].is<bool>()) {
            enabled = doc["tcp_enabled"].as<bool>();
        }
        if (valid_port_from_json(doc, "tcp_port", parsed_port)) {
            port = parsed_port;
        }
        const bool before_enabled = config.data().tcp_bridge_enabled;
        const uint16_t before_port = config.data().tcp_bridge_port;
        const bool accepted = config.set_tcp_bridge(enabled, port);
        if (accepted) {
            result.accepted_fields++;
            if (before_enabled != config.data().tcp_bridge_enabled ||
                before_port != config.data().tcp_bridge_port) {
                result.changed_fields++;
            }
        }
    }

    if (doc["telnet_enabled"].is<bool>() ||
        valid_port_from_json(doc, "telnet_port", parsed_port)) {
        bool enabled = config.data().telnet_console_enabled;
        uint16_t port = config.data().telnet_console_port;
        if (doc["telnet_enabled"].is<bool>()) {
            enabled = doc["telnet_enabled"].as<bool>();
        }
        if (valid_port_from_json(doc, "telnet_port", parsed_port)) {
            port = parsed_port;
        }
        const bool before_enabled = config.data().telnet_console_enabled;
        const uint16_t before_port = config.data().telnet_console_port;
        const bool accepted = config.set_telnet_console(enabled, port);
        if (accepted) {
            result.accepted_fields++;
            if (before_enabled != config.data().telnet_console_enabled ||
                before_port != config.data().telnet_console_port) {
                result.changed_fields++;
            }
        }
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
    if (result.edf_capture_changed) {
        edf_recorder_manager.set_enabled(config.data().edf_capture_enabled);
    }
    if (result.wifi_reconnect_required) {
        wifi_manager.reconnect();
    }
}

}  // namespace aircannect
