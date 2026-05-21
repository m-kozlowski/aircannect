#include "app_config.h"

#include <IPAddress.h>
#include <Preferences.h>
#include <ctype.h>

#include "debug_log.h"

namespace aircannect {

namespace {

static constexpr const char *CFG_NS = "cfg";
static constexpr const char *KEY_SCHEMA = "schema";
static constexpr const char *KEY_HOSTNAME = "host";
static constexpr const char *KEY_TCP_ENABLED = "tcp_en";
static constexpr const char *KEY_TCP_PORT = "tcp_port";
static constexpr const char *KEY_SOFTAP_LEGACY = "softap";
static constexpr const char *KEY_SOFTAP_MODE = "softap_mode";
static constexpr const char *KEY_WIFI_COUNTRY = "wifi_ctry";
static constexpr const char *KEY_TIMEZONE = "tz";
static constexpr const char *KEY_RESMED_TIME_SYNC = "resmed_time";
static constexpr const char *KEY_OXIMETRY_ENABLED = "oxi_en";
static constexpr const char *KEY_OXIMETRY_UDP_PORT = "oxi_udp";
static constexpr const char *KEY_OXIMETRY_ADVERTISE_MODE = "oxi_adv";
static constexpr const char *KEY_HTTP_USER = "http_user";
static constexpr const char *KEY_HTTP_PASSWORD = "http_pass";
static constexpr const char *KEY_AUTH_WHITELIST = "auth_wl";
static constexpr const char *KEY_TELNET_ENABLED = "telnet_en";
static constexpr const char *KEY_TELNET_PORT = "telnet_port";
static constexpr const char *KEY_OTA_PASSWORD = "ota_pass";
static constexpr const char *KEY_SYSLOG_ENABLED = "syslog_en";
static constexpr const char *KEY_SYSLOG_HOST = "syslog_host";
static constexpr const char *KEY_SYSLOG_PORT = "syslog_port";

static constexpr uint32_t DIRTY_HOSTNAME = 1UL << 0;
static constexpr uint32_t DIRTY_TCP = 1UL << 1;
static constexpr uint32_t DIRTY_SOFTAP = 1UL << 2;
static constexpr uint32_t DIRTY_WIFI_COUNTRY = 1UL << 3;
static constexpr uint32_t DIRTY_TIMEZONE = 1UL << 4;
static constexpr uint32_t DIRTY_RESMED_TIME = 1UL << 5;
static constexpr uint32_t DIRTY_HTTP_AUTH = 1UL << 6;
static constexpr uint32_t DIRTY_AUTH_WHITELIST = 1UL << 7;
static constexpr uint32_t DIRTY_TELNET = 1UL << 8;
static constexpr uint32_t DIRTY_OTA_PASSWORD = 1UL << 9;
static constexpr uint32_t DIRTY_OXIMETRY = 1UL << 10;
static constexpr uint32_t DIRTY_LOG_LEVELS = 1UL << 12;
static constexpr uint32_t DIRTY_SYSLOG = 1UL << 13;
static constexpr uint32_t DIRTY_ALL =
    DIRTY_HOSTNAME | DIRTY_TCP | DIRTY_SOFTAP | DIRTY_WIFI_COUNTRY |
    DIRTY_TIMEZONE | DIRTY_RESMED_TIME | DIRTY_HTTP_AUTH |
    DIRTY_AUTH_WHITELIST | DIRTY_TELNET | DIRTY_OTA_PASSWORD |
    DIRTY_OXIMETRY | DIRTY_LOG_LEVELS | DIRTY_SYSLOG;

bool valid_hostname_char(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '-';
}

bool valid_hostname(const String &hostname) {
    const size_t len = hostname.length();
    if (len == 0 || len > 63) return false;
    if (hostname[0] == '-' || hostname[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i) {
        if (!valid_hostname_char(hostname[i])) return false;
    }
    return true;
}

bool valid_timezone(const String &timezone) {
    if (!timezone.length() || timezone.length() > 64) return false;
    for (size_t i = 0; i < timezone.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(timezone[i]);
        if (c < 0x20 || c >= 0x7F) return false;
    }
    return true;
}

bool valid_secret(const String &secret) {
    if (!secret.length() || secret.length() > 64) return false;
    for (size_t i = 0; i < secret.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(secret[i]);
        if (c < 0x20 || c >= 0x7F) return false;
    }
    return true;
}

bool valid_optional_secret(const String &secret) {
    if (secret.length() > 64) return false;
    for (size_t i = 0; i < secret.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(secret[i]);
        if (c < 0x20 || c >= 0x7F) return false;
    }
    return true;
}

bool valid_log_level(log_level_t level) {
    return level >= LOG_ERROR && level <= LOG_DEBUG;
}

bool valid_syslog_host(const String &host) {
    if (!host.length()) return true;
    IPAddress ip;
    return ip.fromString(host);
}

bool valid_auth_whitelist(const String &whitelist) {
    if (whitelist.length() > 160) return false;
    for (size_t i = 0; i < whitelist.length(); ++i) {
        const unsigned char c = static_cast<unsigned char>(whitelist[i]);
        if (c < 0x20 || c >= 0x7F) return false;
        if (!isdigit(c) && c != '.' && c != '/' && c != '-' && c != ',' &&
            c != '*' && c != ' ') {
            return false;
        }
    }
    return true;
}

bool valid_wifi_country(const String &country) {
    if (country == "01") return true;
    if (country.length() != 2) return false;
    for (size_t i = 0; i < country.length(); ++i) {
        if (!isalpha(static_cast<unsigned char>(country[i]))) return false;
    }
    return true;
}

OximetryAdvertiseMode default_oximetry_advertise_mode() {
    const OximetryAdvertiseMode mode =
        static_cast<OximetryAdvertiseMode>(
            AC_DEFAULT_OXIMETRY_ADVERTISE_MODE);
    if (oximetry_advertise_mode_valid(mode)) return mode;
    return OximetryAdvertiseMode::Auto;
}

OximetryAdvertiseMode load_oximetry_advertise_mode(
    Preferences &prefs,
    OximetryAdvertiseMode fallback) {
    const PreferenceType type = prefs.getType(KEY_OXIMETRY_ADVERTISE_MODE);
    if (type == PT_U8) {
        const OximetryAdvertiseMode mode =
            static_cast<OximetryAdvertiseMode>(
                prefs.getUChar(KEY_OXIMETRY_ADVERTISE_MODE,
                               static_cast<uint8_t>(fallback)));
        if (oximetry_advertise_mode_valid(mode)) return mode;
    } else if (type == PT_STR) {
        OximetryAdvertiseMode mode = fallback;
        if (parse_oximetry_advertise_mode(
                prefs.getString(KEY_OXIMETRY_ADVERTISE_MODE, ""),
                mode)) {
            return mode;
        }
    }
    return fallback;
}

void apply_build_defaults(AppConfigData &data) {
    data.oximetry_advertise_mode = default_oximetry_advertise_mode();
}

const char *on_off(bool enabled) {
    return enabled ? "on" : "off";
}

bool put_string(Preferences &prefs, const char *key, const String &value) {
    const size_t written = prefs.putString(key, value);
    return written != 0 || value.length() == 0;
}

}  // namespace

bool AppConfig::begin() {
    if (!load()) {
        set_defaults();
        save();
        return false;
    }
    return true;
}

bool AppConfig::load() {
    AppConfigData defaults;
    apply_build_defaults(defaults);

    Preferences prefs;
    if (!prefs.begin(CFG_NS, true)) {
        Log::logf(CAT_GENERAL, LOG_WARN, "[CONFIG] failed to open NVS\n");
        return false;
    }

    const uint32_t schema = prefs.getUInt(KEY_SCHEMA, 0);
    if (schema == 0) {
        prefs.end();
        set_defaults();
        return save();
    }

    if (schema > AC_CONFIG_SCHEMA_VERSION) {
        prefs.end();
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[CONFIG] future schema %lu; using defaults\n",
                  static_cast<unsigned long>(schema));
        set_defaults();
        return save();
    }

    data_.schema_version = schema;
    data_.hostname = prefs.getString(KEY_HOSTNAME, defaults.hostname);
    data_.tcp_bridge_enabled =
        prefs.getBool(KEY_TCP_ENABLED, defaults.tcp_bridge_enabled);
    data_.tcp_bridge_port = static_cast<uint16_t>(
        prefs.getUInt(KEY_TCP_PORT, defaults.tcp_bridge_port));
    String softap_mode = prefs.getString(KEY_SOFTAP_MODE, "");
    if (!parse_softap_mode(softap_mode, data_.softap_mode)) {
        // The old boolean had no "forced" state. Migrate either legacy value
        // to auto so saved devices keep STA-first AP recovery behavior.
        (void)prefs.getBool(KEY_SOFTAP_LEGACY,
                            AC_ENABLE_SOFTAP_FALLBACK != 0);
        data_.softap_mode = SoftApMode::Auto;
    }
    data_.wifi_country =
        prefs.getString(KEY_WIFI_COUNTRY, defaults.wifi_country);
    data_.timezone = prefs.getString(KEY_TIMEZONE, defaults.timezone);
    data_.resmed_time_sync_enabled =
        prefs.getBool(KEY_RESMED_TIME_SYNC,
                      defaults.resmed_time_sync_enabled);
    data_.oximetry_enabled =
        prefs.getBool(KEY_OXIMETRY_ENABLED, defaults.oximetry_enabled);
    data_.oximetry_udp_port = static_cast<uint16_t>(
        prefs.getUInt(KEY_OXIMETRY_UDP_PORT, defaults.oximetry_udp_port));
    data_.oximetry_advertise_mode = load_oximetry_advertise_mode(
        prefs, defaults.oximetry_advertise_mode);
    data_.http_user = prefs.getString(KEY_HTTP_USER, defaults.http_user);
    data_.http_password =
        prefs.getString(KEY_HTTP_PASSWORD, defaults.http_password);
    data_.auth_whitelist =
        prefs.getString(KEY_AUTH_WHITELIST, defaults.auth_whitelist);
    data_.telnet_console_enabled =
        prefs.getBool(KEY_TELNET_ENABLED, defaults.telnet_console_enabled);
    data_.telnet_console_port = static_cast<uint16_t>(
        prefs.getUInt(KEY_TELNET_PORT, defaults.telnet_console_port));
    data_.ota_password =
        prefs.getString(KEY_OTA_PASSWORD, defaults.ota_password);
    for (int i = 0; i < CAT_COUNT; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "log%u", static_cast<unsigned>(i));
        data_.log_levels[i] =
            static_cast<log_level_t>(
                prefs.getUChar(key, defaults.log_levels[i]));
    }
    data_.syslog_enabled =
        prefs.getBool(KEY_SYSLOG_ENABLED, defaults.syslog_enabled);
    data_.syslog_host = prefs.getString(KEY_SYSLOG_HOST, defaults.syslog_host);
    data_.syslog_port = static_cast<uint16_t>(
        prefs.getUInt(KEY_SYSLOG_PORT, defaults.syslog_port));
    prefs.end();

    if (!normalize()) return save();
    return true;
}

bool AppConfig::save() const {
    return save_fields(DIRTY_ALL);
}

bool AppConfig::save_fields(uint32_t dirty) const {
    if (!dirty) return true;

    Preferences prefs;
    if (!prefs.begin(CFG_NS, false)) {
        Log::logf(CAT_GENERAL, LOG_ERROR, "[CONFIG] failed to open NVS RW\n");
        return false;
    }

    bool ok = true;
    ok = prefs.putUInt(KEY_SCHEMA, data_.schema_version) != 0 && ok;
    if (dirty & DIRTY_HOSTNAME) {
        ok = put_string(prefs, KEY_HOSTNAME, data_.hostname) && ok;
    }
    if (dirty & DIRTY_TCP) {
        ok = prefs.putBool(KEY_TCP_ENABLED, data_.tcp_bridge_enabled) != 0 &&
             ok;
        ok = prefs.putUInt(KEY_TCP_PORT, data_.tcp_bridge_port) != 0 && ok;
    }
    if (dirty & DIRTY_SOFTAP) {
        ok = put_string(prefs, KEY_SOFTAP_MODE,
                        softap_mode_name(data_.softap_mode)) && ok;
    }
    if (dirty & DIRTY_WIFI_COUNTRY) {
        ok = put_string(prefs, KEY_WIFI_COUNTRY, data_.wifi_country) && ok;
    }
    if (dirty & DIRTY_TIMEZONE) {
        ok = put_string(prefs, KEY_TIMEZONE, data_.timezone) && ok;
    }
    if (dirty & DIRTY_RESMED_TIME) {
        ok = prefs.putBool(KEY_RESMED_TIME_SYNC,
                           data_.resmed_time_sync_enabled) != 0 &&
             ok;
    }
    if (dirty & DIRTY_OXIMETRY) {
        ok = prefs.putBool(KEY_OXIMETRY_ENABLED,
                           data_.oximetry_enabled) != 0 &&
             ok;
        ok = prefs.putUInt(KEY_OXIMETRY_UDP_PORT,
                           data_.oximetry_udp_port) != 0 &&
             ok;
        ok = prefs.putUChar(
                 KEY_OXIMETRY_ADVERTISE_MODE,
                 static_cast<uint8_t>(data_.oximetry_advertise_mode)) != 0 &&
             ok;
    }
    if (dirty & DIRTY_HTTP_AUTH) {
        ok = put_string(prefs, KEY_HTTP_USER, data_.http_user) && ok;
        ok = put_string(prefs, KEY_HTTP_PASSWORD, data_.http_password) && ok;
    }
    if (dirty & DIRTY_AUTH_WHITELIST) {
        ok = put_string(prefs, KEY_AUTH_WHITELIST, data_.auth_whitelist) && ok;
    }
    if (dirty & DIRTY_TELNET) {
        ok = prefs.putBool(KEY_TELNET_ENABLED,
                           data_.telnet_console_enabled) != 0 &&
             ok;
        ok = prefs.putUInt(KEY_TELNET_PORT, data_.telnet_console_port) != 0 &&
             ok;
    }
    if (dirty & DIRTY_OTA_PASSWORD) {
        ok = put_string(prefs, KEY_OTA_PASSWORD, data_.ota_password) && ok;
    }
    if (dirty & DIRTY_LOG_LEVELS) {
        for (int i = 0; i < CAT_COUNT; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "log%u", static_cast<unsigned>(i));
            ok = prefs.putUChar(key, data_.log_levels[i]) != 0 && ok;
        }
    }
    if (dirty & DIRTY_SYSLOG) {
        ok = prefs.putBool(KEY_SYSLOG_ENABLED, data_.syslog_enabled) != 0 &&
             ok;
        ok = put_string(prefs, KEY_SYSLOG_HOST, data_.syslog_host) && ok;
        ok = prefs.putUInt(KEY_SYSLOG_PORT, data_.syslog_port) != 0 && ok;
    }
    prefs.end();

    if (!ok) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[CONFIG] one or more values were not persisted\n");
    }
    return ok;
}

bool AppConfig::persist(uint32_t dirty) {
    if (!dirty) return true;
    pending_dirty_ |= dirty;
    if (update_depth_ > 0) return true;

    const uint32_t to_save = pending_dirty_;
    if (!save_fields(to_save)) return false;
    pending_dirty_ &= ~to_save;
    return true;
}

void AppConfig::begin_update() {
    update_depth_++;
}

bool AppConfig::commit_update() {
    if (update_depth_ > 0) update_depth_--;
    if (update_depth_ > 0) return true;
    const uint32_t to_save = pending_dirty_;
    if (!to_save) return true;
    if (!save_fields(to_save)) return false;
    pending_dirty_ &= ~to_save;
    return true;
}

void AppConfig::set_defaults() {
    data_ = AppConfigData{};
    apply_build_defaults(data_);
}

bool AppConfig::normalize() {
    AppConfigData defaults;
    apply_build_defaults(defaults);
    bool unchanged = true;
    if (data_.schema_version != AC_CONFIG_SCHEMA_VERSION) {
        data_.schema_version = AC_CONFIG_SCHEMA_VERSION;
        unchanged = false;
    }
    if (!valid_hostname(data_.hostname)) {
        data_.hostname = defaults.hostname;
        unchanged = false;
    }
    if (data_.tcp_bridge_port == 0) {
        data_.tcp_bridge_port = defaults.tcp_bridge_port;
        unchanged = false;
    }
    if (!softap_mode_valid(data_.softap_mode)) {
        data_.softap_mode = SoftApMode::Auto;
        unchanged = false;
    }
    if (!valid_wifi_country(data_.wifi_country)) {
        data_.wifi_country = defaults.wifi_country;
        unchanged = false;
    } else {
        data_.wifi_country.toUpperCase();
    }
    if (!valid_timezone(data_.timezone)) {
        data_.timezone = defaults.timezone;
        unchanged = false;
    }
    if (data_.oximetry_udp_port == 0) {
        data_.oximetry_udp_port = defaults.oximetry_udp_port;
        unchanged = false;
    }
    if (!oximetry_advertise_mode_valid(data_.oximetry_advertise_mode)) {
        data_.oximetry_advertise_mode =
            defaults.oximetry_advertise_mode;
        unchanged = false;
    }
    if (!valid_optional_secret(data_.http_user)) {
        data_.http_user = defaults.http_user;
        unchanged = false;
    }
    if (!valid_optional_secret(data_.http_password)) {
        data_.http_password = defaults.http_password;
        unchanged = false;
    }
    if (!valid_auth_whitelist(data_.auth_whitelist)) {
        data_.auth_whitelist = "";
        unchanged = false;
    }
    if (data_.telnet_console_port == 0) {
        data_.telnet_console_port = defaults.telnet_console_port;
        unchanged = false;
    }
    if (!valid_optional_secret(data_.ota_password)) {
        data_.ota_password = defaults.ota_password;
        unchanged = false;
    }
    for (int i = 0; i < CAT_COUNT; ++i) {
        if (!valid_log_level(data_.log_levels[i])) {
            data_.log_levels[i] = defaults.log_levels[i];
            unchanged = false;
        }
    }
    data_.syslog_host.trim();
    if (!valid_syslog_host(data_.syslog_host)) {
        data_.syslog_host = "";
        data_.syslog_enabled = false;
        unchanged = false;
    }
    if (data_.syslog_port == 0) {
        data_.syslog_port = defaults.syslog_port;
        unchanged = false;
    }
    if (data_.syslog_enabled && !data_.syslog_host.length()) {
        data_.syslog_enabled = false;
        unchanged = false;
    }
    return unchanged;
}

bool AppConfig::set_hostname(const String &hostname) {
    String value = hostname;
    value.trim();
    if (!valid_hostname(value)) return false;
    if (data_.hostname == value) return true;
    data_.hostname = value;
    return persist(DIRTY_HOSTNAME);
}

bool AppConfig::set_tcp_bridge(bool enabled, uint16_t port) {
    if (port == 0) return false;
    if (data_.tcp_bridge_enabled == enabled && data_.tcp_bridge_port == port) {
        return true;
    }
    data_.tcp_bridge_enabled = enabled;
    data_.tcp_bridge_port = port;
    return persist(DIRTY_TCP);
}

bool AppConfig::set_softap_mode(SoftApMode mode) {
    if (!softap_mode_valid(mode)) return false;
    if (data_.softap_mode == mode) return true;
    data_.softap_mode = mode;
    return persist(DIRTY_SOFTAP);
}

bool AppConfig::set_wifi_country(const String &country) {
    String value = country;
    value.trim();
    value.toUpperCase();
    if (value == "CLEAR" || value == "OFF" || value == "NONE" ||
        value == "DEFAULT" || value == "WORLDWIDE") {
        value = "01";
    }
    if (!valid_wifi_country(value)) return false;
    if (data_.wifi_country == value) return true;
    data_.wifi_country = value;
    return persist(DIRTY_WIFI_COUNTRY);
}

bool AppConfig::set_timezone(const String &timezone) {
    String value = timezone;
    value.trim();
    if (!valid_timezone(value)) return false;
    if (data_.timezone == value) return true;
    data_.timezone = value;
    return persist(DIRTY_TIMEZONE);
}

bool AppConfig::set_resmed_time_sync(bool enabled) {
    if (data_.resmed_time_sync_enabled == enabled) return true;
    data_.resmed_time_sync_enabled = enabled;
    return persist(DIRTY_RESMED_TIME);
}

bool AppConfig::set_oximetry_enabled(bool enabled) {
    if (data_.oximetry_enabled == enabled) return true;
    data_.oximetry_enabled = enabled;
    return persist(DIRTY_OXIMETRY);
}

bool AppConfig::set_oximetry_udp_port(uint16_t port) {
    if (port == 0) return false;
    if (data_.oximetry_udp_port == port) return true;
    data_.oximetry_udp_port = port;
    return persist(DIRTY_OXIMETRY);
}

bool AppConfig::set_oximetry_advertise_mode(
    OximetryAdvertiseMode mode) {
    if (!oximetry_advertise_mode_valid(mode)) return false;
    if (data_.oximetry_advertise_mode == mode) return true;
    data_.oximetry_advertise_mode = mode;
    return persist(DIRTY_OXIMETRY);
}

bool AppConfig::set_http_auth(const String &user, const String &password) {
    String parsed_user = user;
    String parsed_password = password;
    parsed_user.trim();
    parsed_password.trim();
    if (!valid_optional_secret(parsed_user) ||
        !valid_optional_secret(parsed_password)) {
        return false;
    }
    if (data_.http_user == parsed_user &&
        data_.http_password == parsed_password) {
        return true;
    }
    data_.http_user = parsed_user;
    data_.http_password = parsed_password;
    return persist(DIRTY_HTTP_AUTH);
}

bool AppConfig::set_auth_whitelist(const String &whitelist) {
    String value = whitelist;
    value.trim();
    String lower = value;
    lower.toLowerCase();
    if (lower == "clear" || lower == "off" || lower == "none") value = "";
    if (!valid_auth_whitelist(value)) return false;
    if (data_.auth_whitelist == value) return true;
    data_.auth_whitelist = value;
    return persist(DIRTY_AUTH_WHITELIST);
}

bool AppConfig::set_telnet_console(bool enabled, uint16_t port) {
    if (port == 0) return false;
    if (data_.telnet_console_enabled == enabled &&
        data_.telnet_console_port == port) {
        return true;
    }
    data_.telnet_console_enabled = enabled;
    data_.telnet_console_port = port;
    return persist(DIRTY_TELNET);
}

bool AppConfig::set_ota_password(const String &password) {
    String value = password;
    value.trim();
    if (!valid_optional_secret(value)) return false;
    if (data_.ota_password == value) return true;
    data_.ota_password = value;
    return persist(DIRTY_OTA_PASSWORD);
}

bool AppConfig::set_log_level(log_cat_t cat, log_level_t level) {
    if (cat < 0 || cat >= CAT_COUNT || !valid_log_level(level)) return false;
    if (data_.log_levels[cat] == level) return true;
    data_.log_levels[cat] = level;
    return persist(DIRTY_LOG_LEVELS);
}

bool AppConfig::set_all_log_levels(log_level_t level) {
    if (!valid_log_level(level)) return false;
    bool changed = false;
    for (int i = 0; i < CAT_COUNT; ++i) {
        if (data_.log_levels[i] != level) changed = true;
    }
    if (!changed) return true;
    for (int i = 0; i < CAT_COUNT; ++i) data_.log_levels[i] = level;
    return persist(DIRTY_LOG_LEVELS);
}

bool AppConfig::set_syslog(bool enabled, const String &host, uint16_t port) {
    String value = host;
    value.trim();
    if (!enabled) value = "";
    if (port == 0) return false;
    if (enabled && (!value.length() || !valid_syslog_host(value))) {
        return false;
    }
    if (data_.syslog_enabled == enabled && data_.syslog_host == value &&
        data_.syslog_port == port) {
        return true;
    }
    data_.syslog_enabled = enabled;
    data_.syslog_host = value;
    data_.syslog_port = port;
    return persist(DIRTY_SYSLOG);
}

bool AppConfig::factory_reset() {
    Preferences prefs;
    bool cleared = false;
    if (prefs.begin(CFG_NS, false)) {
        cleared = prefs.clear();
        prefs.end();
    }
    set_defaults();
    pending_dirty_ = 0;
    update_depth_ = 0;
    return save() && cleared;
}

void AppConfig::apply_log_config() const {
    for (int i = 0; i < CAT_COUNT; ++i) {
        Log::set_cat_level(static_cast<log_cat_t>(i), data_.log_levels[i]);
    }
    Log::configure_syslog(data_.syslog_enabled, data_.syslog_host,
                          data_.syslog_port, data_.hostname);
}

void AppConfig::print_redacted(Print &out) const {
    out.println("[CONFIG]");
    out.print("  schema: ");
    out.println(data_.schema_version);
    out.print("  hostname: ");
    out.println(data_.hostname);
    out.print("  tcp: ");
    out.print(on_off(data_.tcp_bridge_enabled));
    out.print(" port=");
    out.println(data_.tcp_bridge_port);
    out.print("  softap: ");
    out.println(softap_mode_name(data_.softap_mode));
    out.print("  wifi_country: ");
    out.println(data_.wifi_country);
    out.print("  timezone: ");
    out.println(data_.timezone);
    out.print("  resmed_time_sync: ");
    out.println(on_off(data_.resmed_time_sync_enabled));
    out.print("  oximetry: ");
    out.print(on_off(data_.oximetry_enabled));
    out.print(" udp_port=");
    out.print(data_.oximetry_udp_port);
    out.print(" advertise=");
    out.println(oximetry_advertise_mode_name(
        data_.oximetry_advertise_mode));
    out.print("  http_auth: ");
    out.println(data_.http_user.length() || data_.http_password.length()
                    ? "protected"
                    : "open");
    out.print("  http_user: ");
    if (data_.http_user.length()) {
        out.println(data_.http_user);
    } else {
        out.println("<empty>");
    }
    out.print("  http_password: ");
    out.println(data_.http_password.length() ? "<set>" : "<empty>");
    out.print("  http_whitelist: ");
    if (data_.auth_whitelist.length()) {
        out.println(data_.auth_whitelist);
    } else {
        out.println("<empty>");
    }
    out.print("  telnet: ");
    out.print(on_off(data_.telnet_console_enabled));
    out.print(" port=");
    out.println(data_.telnet_console_port);
    out.print("  ota: ");
    out.println(data_.ota_password.length() ? "protected" : "open");
    out.print("  ota_password: ");
    out.println(data_.ota_password.length() ? "<set>" : "<empty>");
}

}  // namespace aircannect
