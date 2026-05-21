#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "board.h"
#include "debug_log.h"
#include "oximetry_types.h"
#include "softap_mode.h"

namespace aircannect {

static constexpr uint32_t AC_CONFIG_SCHEMA_VERSION = 13;

struct AppConfigData {
    uint32_t schema_version = AC_CONFIG_SCHEMA_VERSION;

    String hostname = AC_HOSTNAME;

    bool tcp_bridge_enabled = AC_DEFAULT_TCP_BRIDGE_ENABLED != 0;
    uint16_t tcp_bridge_port = AC_TCP_BRIDGE_PORT;
    bool telnet_console_enabled = AC_DEFAULT_TELNET_CONSOLE_ENABLED != 0;
    uint16_t telnet_console_port = AC_TELNET_CONSOLE_PORT;
    String ota_password = AC_DEFAULT_OTA_PASSWORD;

    String http_user = AC_DEFAULT_HTTP_USER;
    String http_password = AC_DEFAULT_HTTP_PASSWORD;
    String auth_whitelist;

    SoftApMode softap_mode = SoftApMode::Auto;
    String wifi_country = AC_DEFAULT_WIFI_COUNTRY;

    String timezone = AC_DEFAULT_TIMEZONE;
    bool resmed_time_sync_enabled =
        AC_DEFAULT_RESMED_TIME_SYNC_ENABLED != 0;

    bool oximetry_enabled = AC_DEFAULT_OXIMETRY_ENABLED != 0;
    uint16_t oximetry_udp_port = AC_OXIMETRY_UDP_PORT;
    OximetryAdvertiseMode oximetry_advertise_mode =
        OximetryAdvertiseMode::Auto;

    bool syslog_enabled = AC_DEFAULT_SYSLOG_ENABLED != 0;
    String syslog_host;
    uint16_t syslog_port = AC_SYSLOG_PORT;

    log_level_t log_levels[CAT_COUNT] = {
        LOG_INFO,
        LOG_INFO,
        LOG_INFO,
        LOG_INFO,
        LOG_INFO,
        LOG_INFO,
        LOG_INFO,
        LOG_INFO,
        LOG_INFO,
    };
};

class AppConfig {
public:
    bool begin();

    const AppConfigData &data() const { return data_; }

    bool set_hostname(const String &hostname);
    bool set_tcp_bridge(bool enabled, uint16_t port);
    bool set_softap_mode(SoftApMode mode);
    bool set_wifi_country(const String &country);

    bool set_timezone(const String &timezone);
    bool set_resmed_time_sync(bool enabled);
    bool set_oximetry_enabled(bool enabled);
    bool set_oximetry_udp_port(uint16_t port);
    bool set_oximetry_advertise_mode(OximetryAdvertiseMode mode);

    bool set_http_auth(const String &user, const String &password);
    bool set_auth_whitelist(const String &whitelist);
    bool set_telnet_console(bool enabled, uint16_t port);
    bool set_ota_password(const String &password);

    bool set_log_level(log_cat_t cat, log_level_t level);
    bool set_all_log_levels(log_level_t level);
    bool set_syslog(bool enabled, const String &host, uint16_t port);

    void begin_update();
    bool commit_update();

    bool factory_reset();
    void apply_log_config() const;
    void print_redacted(Print &out) const;

private:
    bool load();
    bool save() const;
    bool save_fields(uint32_t dirty) const;
    bool persist(uint32_t dirty);
    void set_defaults();
    bool normalize();

    AppConfigData data_;
    uint32_t pending_dirty_ = 0;
    uint16_t update_depth_ = 0;
};

}  // namespace aircannect
