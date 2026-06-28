#include "app_config_registry.h"

#include <string.h>

#if AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO
#include "string_util.h"
#endif

namespace aircannect {

namespace {

enum ConfigLogCategoryIndex : int16_t {
    CONFIG_LOG_GENERAL = 0,
    CONFIG_LOG_CAN = 1,
    CONFIG_LOG_RPC = 2,
    CONFIG_LOG_TCP = 3,
    CONFIG_LOG_CLI = 4,
    CONFIG_LOG_WIFI = 5,
    CONFIG_LOG_STREAM = 6,
    CONFIG_LOG_OTA = 7,
    CONFIG_LOG_OXI = 8,
    CONFIG_LOG_STORAGE = 9,
    CONFIG_LOG_BGWORKER = 10,
    CONFIG_LOG_REPORT = 11,
    CONFIG_LOG_EDF = 12,
    CONFIG_LOG_CONFIG = 13,
    CONFIG_LOG_SLEEPHQ = 14,
    CONFIG_LOG_COUNT = 15,
};

#if AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO
static_assert(static_cast<int>(CONFIG_LOG_COUNT) == static_cast<int>(CAT_COUNT),
              "update log category config descriptors");
static_assert(static_cast<int>(CONFIG_LOG_GENERAL) ==
                  static_cast<int>(CAT_GENERAL),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_CAN) == static_cast<int>(CAT_CAN),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_RPC) == static_cast<int>(CAT_RPC),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_TCP) == static_cast<int>(CAT_TCP),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_CLI) == static_cast<int>(CAT_CLI),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_WIFI) == static_cast<int>(CAT_WIFI),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_STREAM) ==
                  static_cast<int>(CAT_STREAM),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_OTA) == static_cast<int>(CAT_OTA),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_OXI) == static_cast<int>(CAT_OXI),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_STORAGE) ==
                  static_cast<int>(CAT_STORAGE),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_BGWORKER) ==
                  static_cast<int>(CAT_BGWORKER),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_REPORT) ==
                  static_cast<int>(CAT_REPORT),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_EDF) == static_cast<int>(CAT_EDF),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_CONFIG) ==
                  static_cast<int>(CAT_CONFIG),
              "log category order drift");
static_assert(static_cast<int>(CONFIG_LOG_SLEEPHQ) ==
                  static_cast<int>(CAT_SLEEPHQ),
              "log category order drift");
#endif

static constexpr AppConfigEnumValue SOFTAP_MODE_VALUES[] = {
    {"auto", "Auto"},
    {"forced", "Forced"},
};

static constexpr AppConfigEnumValue OXIMETRY_ADVERTISE_VALUES[] = {
    {"auto", "Auto"},
    {"manual", "Manual"},
};

static constexpr AppConfigFieldFlags PROVISIONABLE =
    AC_CONFIG_FIELD_PROVISIONABLE;
static constexpr AppConfigFieldFlags SECRET_PROVISIONABLE =
    static_cast<AppConfigFieldFlags>(AC_CONFIG_FIELD_SECRET |
                                     AC_CONFIG_FIELD_PROVISIONABLE);

#if AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO
#define AC_CFG_OFFSET(member) offsetof(AppConfigData, member)
#else
#define AC_CFG_OFFSET(member) 0
#endif

#define AC_LOG_FIELD(key, order, label, index)                               \
    {key, AppConfigFieldId::LogLevel, AppConfigGroup::Logging, order,        \
     AppConfigFieldType::LogLevel, PROVISIONABLE, AC_CONFIG_DIRTY_LOG_LEVELS,\
     label " log level", label " log category level.", nullptr, 0, index,   \
     AC_CFG_OFFSET(log_levels)}

static constexpr AppConfigFieldDescriptor CONFIG_FIELDS[] = {
    {"host", AppConfigFieldId::Hostname, AppConfigGroup::Device, 10,
     AppConfigFieldType::String, PROVISIONABLE, AC_CONFIG_DIRTY_HOSTNAME,
     "Hostname", "Device hostname.", nullptr, 0, -1,
     AC_CFG_OFFSET(hostname)},
    {"edf_cap", AppConfigFieldId::EdfCaptureEnabled, AppConfigGroup::Device,
     20, AppConfigFieldType::Bool, PROVISIONABLE,
     AC_CONFIG_DIRTY_EDF_CAPTURE, "EDF capture", "Enable EDF capture.",
     nullptr, 0, -1, AC_CFG_OFFSET(edf_capture_enabled)},

    {"softap_mode", AppConfigFieldId::SoftApMode, AppConfigGroup::Network, 10,
     AppConfigFieldType::Enum, PROVISIONABLE, AC_CONFIG_DIRTY_SOFTAP,
     "SoftAP mode", "Fallback access point mode.", SOFTAP_MODE_VALUES,
     sizeof(SOFTAP_MODE_VALUES) / sizeof(SOFTAP_MODE_VALUES[0]), -1,
     AC_CFG_OFFSET(softap_mode)},
    {"wifi_ctry", AppConfigFieldId::WifiCountry, AppConfigGroup::Network, 20,
     AppConfigFieldType::String, PROVISIONABLE, AC_CONFIG_DIRTY_WIFI_COUNTRY,
     "Wi-Fi country", "Two-letter regulatory country code.", nullptr, 0, -1,
     AC_CFG_OFFSET(wifi_country)},

    {"tcp_en", AppConfigFieldId::TcpEnabled, AppConfigGroup::Access, 10,
     AppConfigFieldType::Bool, PROVISIONABLE, AC_CONFIG_DIRTY_TCP,
     "TCP bridge", "Enable raw TCP AS11 bridge.", nullptr, 0, -1,
     AC_CFG_OFFSET(tcp_bridge_enabled)},
    {"tcp_port", AppConfigFieldId::TcpPort, AppConfigGroup::Access, 20,
     AppConfigFieldType::UInt16, PROVISIONABLE, AC_CONFIG_DIRTY_TCP,
     "TCP bridge port", "Raw TCP AS11 bridge port.", nullptr, 0, -1,
     AC_CFG_OFFSET(tcp_bridge_port)},
    {"telnet_en", AppConfigFieldId::TelnetEnabled, AppConfigGroup::Access, 30,
     AppConfigFieldType::Bool, PROVISIONABLE, AC_CONFIG_DIRTY_TELNET,
     "Telnet console", "Enable telnet management console.", nullptr, 0, -1,
     AC_CFG_OFFSET(telnet_console_enabled)},
    {"telnet_port", AppConfigFieldId::TelnetPort, AppConfigGroup::Access, 40,
     AppConfigFieldType::UInt16, PROVISIONABLE, AC_CONFIG_DIRTY_TELNET,
     "Telnet port", "Telnet management console port.", nullptr, 0, -1,
     AC_CFG_OFFSET(telnet_console_port)},
    {"http_user", AppConfigFieldId::HttpUser, AppConfigGroup::Access, 50,
     AppConfigFieldType::String, PROVISIONABLE, AC_CONFIG_DIRTY_HTTP_AUTH,
     "HTTP user", "HTTP basic-auth username.", nullptr, 0, -1,
     AC_CFG_OFFSET(http_user)},
    {"http_pass", AppConfigFieldId::HttpPassword, AppConfigGroup::Access, 60,
     AppConfigFieldType::Secret, SECRET_PROVISIONABLE,
     AC_CONFIG_DIRTY_HTTP_AUTH, "HTTP password", "HTTP basic-auth password.",
     nullptr, 0, -1, AC_CFG_OFFSET(http_password)},
    {"auth_wl", AppConfigFieldId::AuthWhitelist, AppConfigGroup::Access, 70,
     AppConfigFieldType::String, PROVISIONABLE,
     AC_CONFIG_DIRTY_AUTH_WHITELIST, "HTTP auth whitelist",
     "Comma-separated auth whitelist.", nullptr, 0, -1,
     AC_CFG_OFFSET(auth_whitelist)},
    {"ota_pass", AppConfigFieldId::OtaPassword, AppConfigGroup::Access, 80,
     AppConfigFieldType::Secret, SECRET_PROVISIONABLE,
     AC_CONFIG_DIRTY_OTA_PASSWORD, "OTA password", "OTA password.", nullptr,
     0, -1, AC_CFG_OFFSET(ota_password)},

    {"syslog_en", AppConfigFieldId::SyslogEnabled, AppConfigGroup::Logging,
     10, AppConfigFieldType::Bool, PROVISIONABLE, AC_CONFIG_DIRTY_SYSLOG,
     "Syslog", "Enable syslog forwarding.", nullptr, 0, -1,
     AC_CFG_OFFSET(syslog_enabled)},
    {"syslog_host", AppConfigFieldId::SyslogHost, AppConfigGroup::Logging, 20,
     AppConfigFieldType::String, PROVISIONABLE, AC_CONFIG_DIRTY_SYSLOG,
     "Syslog host", "Syslog server IPv4 address.", nullptr, 0, -1,
     AC_CFG_OFFSET(syslog_host)},
    {"syslog_port", AppConfigFieldId::SyslogPort, AppConfigGroup::Logging, 30,
     AppConfigFieldType::UInt16, PROVISIONABLE, AC_CONFIG_DIRTY_SYSLOG,
     "Syslog port", "Syslog UDP port.", nullptr, 0, -1,
     AC_CFG_OFFSET(syslog_port)},
    {"file_log_en", AppConfigFieldId::FileLogEnabled, AppConfigGroup::Logging,
     40, AppConfigFieldType::Bool, PROVISIONABLE, AC_CONFIG_DIRTY_FILE_LOG,
     "File logging", "Enable SD-card file logging.", nullptr, 0, -1,
     AC_CFG_OFFSET(file_log_enabled)},
    AC_LOG_FIELD("log_general", 100, "General", CONFIG_LOG_GENERAL),
    AC_LOG_FIELD("log_can", 101, "CAN", CONFIG_LOG_CAN),
    AC_LOG_FIELD("log_rpc", 102, "RPC", CONFIG_LOG_RPC),
    AC_LOG_FIELD("log_tcp", 103, "TCP", CONFIG_LOG_TCP),
    AC_LOG_FIELD("log_cli", 104, "CLI", CONFIG_LOG_CLI),
    AC_LOG_FIELD("log_wifi", 105, "Wi-Fi", CONFIG_LOG_WIFI),
    AC_LOG_FIELD("log_stream", 106, "Stream", CONFIG_LOG_STREAM),
    AC_LOG_FIELD("log_ota", 107, "OTA", CONFIG_LOG_OTA),
    AC_LOG_FIELD("log_oxi", 108, "Oximetry", CONFIG_LOG_OXI),
    AC_LOG_FIELD("log_storage", 109, "Storage", CONFIG_LOG_STORAGE),
    AC_LOG_FIELD("log_bgworker", 110, "Background worker",
                 CONFIG_LOG_BGWORKER),
    AC_LOG_FIELD("log_report", 111, "Report", CONFIG_LOG_REPORT),
    AC_LOG_FIELD("log_edf", 112, "EDF", CONFIG_LOG_EDF),
    AC_LOG_FIELD("log_config", 113, "Config", CONFIG_LOG_CONFIG),
    AC_LOG_FIELD("log_sleephq", 114, "SleepHQ", CONFIG_LOG_SLEEPHQ),

    {"tz", AppConfigFieldId::Timezone, AppConfigGroup::Time, 10,
     AppConfigFieldType::String, PROVISIONABLE, AC_CONFIG_DIRTY_TIMEZONE,
     "Timezone", "POSIX timezone string.", nullptr, 0, -1,
     AC_CFG_OFFSET(timezone)},
    {"resmed_time", AppConfigFieldId::ResMedTimeSync, AppConfigGroup::Time,
     20, AppConfigFieldType::Bool, PROVISIONABLE,
     AC_CONFIG_DIRTY_RESMED_TIME, "AS11 time sync",
     "Synchronize AS11 clock.", nullptr, 0, -1,
     AC_CFG_OFFSET(resmed_time_sync_enabled)},

    {"oxi_en", AppConfigFieldId::OximetryEnabled, AppConfigGroup::Oximetry,
     10, AppConfigFieldType::Bool, PROVISIONABLE, AC_CONFIG_DIRTY_OXIMETRY,
     "Oximetry", "Enable oximetry support.", nullptr, 0, -1,
     AC_CFG_OFFSET(oximetry_enabled)},
    {"oxi_udp", AppConfigFieldId::OximetryUdpPort, AppConfigGroup::Oximetry,
     20, AppConfigFieldType::UInt16, PROVISIONABLE,
     AC_CONFIG_DIRTY_OXIMETRY, "Oximetry UDP port",
     "Oximetry UDP listener port.", nullptr, 0, -1,
     AC_CFG_OFFSET(oximetry_udp_port)},
    {"oxi_adv", AppConfigFieldId::OximetryAdvertiseMode,
     AppConfigGroup::Oximetry, 30, AppConfigFieldType::Enum, PROVISIONABLE,
     AC_CONFIG_DIRTY_OXIMETRY, "BLE advertising",
     "Oximetry BLE advertising mode.", OXIMETRY_ADVERTISE_VALUES,
     sizeof(OXIMETRY_ADVERTISE_VALUES) /
         sizeof(OXIMETRY_ADVERTISE_VALUES[0]),
     -1, AC_CFG_OFFSET(oximetry_advertise_mode)},

    {"smb_ep", AppConfigFieldId::SmbEndpoint, AppConfigGroup::Smb, 10,
     AppConfigFieldType::String, PROVISIONABLE, AC_CONFIG_DIRTY_SMB_SYNC,
     "SMB endpoint", "SMB UNC endpoint.", nullptr, 0, -1,
     AC_CFG_OFFSET(smb_endpoint)},
    {"smb_user", AppConfigFieldId::SmbUser, AppConfigGroup::Smb, 20,
     AppConfigFieldType::String, PROVISIONABLE, AC_CONFIG_DIRTY_SMB_SYNC,
     "SMB user", "SMB username.", nullptr, 0, -1,
     AC_CFG_OFFSET(smb_user)},
    {"smb_pass", AppConfigFieldId::SmbPassword, AppConfigGroup::Smb, 30,
     AppConfigFieldType::Secret, SECRET_PROVISIONABLE,
     AC_CONFIG_DIRTY_SMB_SYNC, "SMB password", "SMB password.", nullptr, 0,
     -1, AC_CFG_OFFSET(smb_password)},

    {"shq_id", AppConfigFieldId::SleepHqClientId, AppConfigGroup::SleepHq,
     10, AppConfigFieldType::String, PROVISIONABLE,
     AC_CONFIG_DIRTY_SLEEPHQ_SYNC, "SleepHQ client ID",
     "SleepHQ OAuth client ID.", nullptr, 0, -1,
     AC_CFG_OFFSET(sleephq_client_id)},
    {"shq_secret", AppConfigFieldId::SleepHqClientSecret,
     AppConfigGroup::SleepHq, 20, AppConfigFieldType::Secret,
     SECRET_PROVISIONABLE, AC_CONFIG_DIRTY_SLEEPHQ_SYNC,
     "SleepHQ client secret", "SleepHQ OAuth client secret.", nullptr, 0,
     -1, AC_CFG_OFFSET(sleephq_client_secret)},
    {"shq_team", AppConfigFieldId::SleepHqTeamId, AppConfigGroup::SleepHq,
     30, AppConfigFieldType::String, PROVISIONABLE,
     AC_CONFIG_DIRTY_SLEEPHQ_SYNC, "SleepHQ team ID", "SleepHQ team ID.",
     nullptr, 0, -1, AC_CFG_OFFSET(sleephq_team_id)},
    {"shq_device", AppConfigFieldId::SleepHqDeviceId,
     AppConfigGroup::SleepHq, 40, AppConfigFieldType::String, PROVISIONABLE,
     AC_CONFIG_DIRTY_SLEEPHQ_SYNC, "SleepHQ device ID",
     "SleepHQ device ID.", nullptr, 0, -1,
     AC_CFG_OFFSET(sleephq_device_id)},
};

#undef AC_LOG_FIELD
#undef AC_CFG_OFFSET

#if AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO
void set_empty_text(String &out) {
    out = "<empty>";
}

void append_uint(String &out, uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(value));
    out = buf;
}

template <typename T>
const T &config_field_ref(const AppConfigData &cfg,
                          const AppConfigFieldDescriptor &field) {
    const auto *base = reinterpret_cast<const uint8_t *>(&cfg);
    return *reinterpret_cast<const T *>(base + field.offset);
}

bool raw_value(const AppConfigData &cfg,
               const AppConfigFieldDescriptor &field,
               String &out) {
    out = "";
    switch (field.type) {
        case AppConfigFieldType::Bool:
            out = config_field_ref<bool>(cfg, field) ? "1" : "0";
            return true;
        case AppConfigFieldType::UInt16:
            append_uint(out, config_field_ref<uint16_t>(cfg, field));
            return true;
        case AppConfigFieldType::String:
        case AppConfigFieldType::Secret:
            out = config_field_ref<String>(cfg, field);
            return true;
        case AppConfigFieldType::Enum:
            switch (field.id) {
                case AppConfigFieldId::SoftApMode:
                    out = softap_mode_name(
                        config_field_ref<SoftApMode>(cfg, field));
                    return true;
                case AppConfigFieldId::OximetryAdvertiseMode:
                    out = oximetry_advertise_mode_name(
                        config_field_ref<OximetryAdvertiseMode>(cfg, field));
                    return true;
                default:
                    return false;
            }
        case AppConfigFieldType::LogLevel:
            if (field.index < 0 || field.index >= CAT_COUNT) return false;
            out = Log::level_name(cfg.log_levels[field.index]);
            return true;
    }
    return false;
}

bool set_field_value(AppConfig &config,
                     const AppConfigFieldDescriptor &field,
                     const String &value,
                     bool keep_secret_sentinel) {
    if (app_config_field_is_secret(field) &&
        value == AC_CONFIG_SECRET_SENTINEL) {
        String current;
        if (raw_value(config.data(), field, current) &&
            app_config_secret_sentinel_should_preserve(
                current.c_str(), value.c_str(), keep_secret_sentinel)) {
            return true;
        }
    }

    bool parsed_bool = false;
    uint16_t parsed_port = 0;
    const AppConfigData &cfg = config.data();
    switch (field.id) {
        case AppConfigFieldId::Hostname:
            return config.set_hostname(value);
        case AppConfigFieldId::TcpEnabled:
            if (!parse_bool_yesno(value, parsed_bool)) return false;
            return config.set_tcp_bridge(parsed_bool, cfg.tcp_bridge_port);
        case AppConfigFieldId::TcpPort:
            if (!parse_port(value, parsed_port)) return false;
            return config.set_tcp_bridge(cfg.tcp_bridge_enabled, parsed_port);
        case AppConfigFieldId::SoftApMode: {
            SoftApMode mode = SoftApMode::Auto;
            if (!parse_softap_mode(value, mode)) return false;
            return config.set_softap_mode(mode);
        }
        case AppConfigFieldId::WifiCountry:
            return config.set_wifi_country(value);
        case AppConfigFieldId::Timezone:
            return config.set_timezone(value);
        case AppConfigFieldId::ResMedTimeSync:
            if (!parse_bool_yesno(value, parsed_bool)) return false;
            return config.set_resmed_time_sync(parsed_bool);
        case AppConfigFieldId::OximetryEnabled:
            if (!parse_bool_yesno(value, parsed_bool)) return false;
            return config.set_oximetry_enabled(parsed_bool);
        case AppConfigFieldId::OximetryUdpPort:
            if (!parse_port(value, parsed_port)) return false;
            return config.set_oximetry_udp_port(parsed_port);
        case AppConfigFieldId::OximetryAdvertiseMode: {
            OximetryAdvertiseMode mode = OximetryAdvertiseMode::Auto;
            if (!parse_oximetry_advertise_mode(value, mode)) return false;
            return config.set_oximetry_advertise_mode(mode);
        }
        case AppConfigFieldId::EdfCaptureEnabled:
            if (!parse_bool_yesno(value, parsed_bool)) return false;
            return config.set_edf_capture_enabled(parsed_bool);
        case AppConfigFieldId::SmbEndpoint:
            return config.set_smb_credentials(value, cfg.smb_user,
                                              cfg.smb_password);
        case AppConfigFieldId::SmbUser:
            return config.set_smb_credentials(cfg.smb_endpoint, value,
                                              cfg.smb_password);
        case AppConfigFieldId::SmbPassword:
            return config.set_smb_credentials(cfg.smb_endpoint, cfg.smb_user,
                                              value);
        case AppConfigFieldId::SleepHqClientId:
            return config.set_sleephq_credentials(
                value, cfg.sleephq_client_secret, cfg.sleephq_team_id,
                cfg.sleephq_device_id);
        case AppConfigFieldId::SleepHqClientSecret:
            return config.set_sleephq_credentials(
                cfg.sleephq_client_id, value, cfg.sleephq_team_id,
                cfg.sleephq_device_id);
        case AppConfigFieldId::SleepHqTeamId:
            return config.set_sleephq_credentials(
                cfg.sleephq_client_id, cfg.sleephq_client_secret, value,
                cfg.sleephq_device_id);
        case AppConfigFieldId::SleepHqDeviceId:
            return config.set_sleephq_credentials(
                cfg.sleephq_client_id, cfg.sleephq_client_secret,
                cfg.sleephq_team_id, value);
        case AppConfigFieldId::HttpUser:
            return config.set_http_auth(value, cfg.http_password);
        case AppConfigFieldId::HttpPassword:
            return config.set_http_auth(cfg.http_user, value);
        case AppConfigFieldId::AuthWhitelist:
            return config.set_auth_whitelist(value);
        case AppConfigFieldId::TelnetEnabled:
            if (!parse_bool_yesno(value, parsed_bool)) return false;
            return config.set_telnet_console(parsed_bool,
                                             cfg.telnet_console_port);
        case AppConfigFieldId::TelnetPort:
            if (!parse_port(value, parsed_port)) return false;
            return config.set_telnet_console(cfg.telnet_console_enabled,
                                             parsed_port);
        case AppConfigFieldId::OtaPassword:
            return config.set_ota_password(value);
        case AppConfigFieldId::SyslogEnabled:
            if (!parse_bool_yesno(value, parsed_bool)) return false;
            return config.set_syslog(parsed_bool, cfg.syslog_host,
                                     cfg.syslog_port);
        case AppConfigFieldId::SyslogHost:
            return config.set_syslog(value.length() > 0, value,
                                     cfg.syslog_port);
        case AppConfigFieldId::SyslogPort:
            if (!parse_port(value, parsed_port)) return false;
            return config.set_syslog(cfg.syslog_enabled, cfg.syslog_host,
                                     parsed_port);
        case AppConfigFieldId::FileLogEnabled:
            if (!parse_bool_yesno(value, parsed_bool)) return false;
            return config.set_file_log(parsed_bool);
        case AppConfigFieldId::LogLevel: {
            if (field.index < 0 || field.index >= CAT_COUNT) return false;
            log_level_t level = LOG_INFO;
            if (!Log::parse_level(value, level)) return false;
            return config.set_log_level(static_cast<log_cat_t>(field.index),
                                        level);
        }
    }
    return false;
}
#endif

}  // namespace

const AppConfigFieldDescriptor *app_config_fields(size_t &count) {
    count = sizeof(CONFIG_FIELDS) / sizeof(CONFIG_FIELDS[0]);
    return CONFIG_FIELDS;
}

const AppConfigFieldDescriptor *app_config_find_field(const char *key) {
    if (!key) return nullptr;
    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(fields[i].key, key) == 0) return &fields[i];
    }
    return nullptr;
}

const char *app_config_group_id(AppConfigGroup group) {
    switch (group) {
        case AppConfigGroup::Device: return "device";
        case AppConfigGroup::Network: return "network";
        case AppConfigGroup::Access: return "access";
        case AppConfigGroup::Logging: return "logging";
        case AppConfigGroup::Time: return "time";
        case AppConfigGroup::Oximetry: return "oximetry";
        case AppConfigGroup::Smb: return "smb";
        case AppConfigGroup::SleepHq: return "sleephq";
    }
    return "unknown";
}

const char *app_config_group_label(AppConfigGroup group) {
    switch (group) {
        case AppConfigGroup::Device: return "Device";
        case AppConfigGroup::Network: return "Network";
        case AppConfigGroup::Access: return "Access";
        case AppConfigGroup::Logging: return "Logging";
        case AppConfigGroup::Time: return "Time";
        case AppConfigGroup::Oximetry: return "Oximetry";
        case AppConfigGroup::Smb: return "SMB";
        case AppConfigGroup::SleepHq: return "SleepHQ";
    }
    return "Unknown";
}

bool app_config_field_is_secret(const AppConfigFieldDescriptor &field) {
    return (field.flags & AC_CONFIG_FIELD_SECRET) != 0 ||
           field.type == AppConfigFieldType::Secret;
}

bool app_config_secret_sentinel_should_preserve(const char *current_value,
                                                const char *incoming_value,
                                                bool keep_secret_sentinel) {
    return keep_secret_sentinel && incoming_value &&
           strcmp(incoming_value, AC_CONFIG_SECRET_SENTINEL) == 0 &&
           current_value && current_value[0] != '\0';
}

#if AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO
bool app_config_field_get_raw_value(const AppConfigData &cfg,
                                    const AppConfigFieldDescriptor &field,
                                    String &out) {
    return raw_value(cfg, field, out);
}

bool app_config_field_get_console_value(const AppConfigData &cfg,
                                        const AppConfigFieldDescriptor &field,
                                        String &out) {
    if (app_config_field_is_secret(field)) {
        String raw;
        if (!raw_value(cfg, field, raw)) return false;
        out = raw.length() ? "<set>" : "<empty>";
        return true;
    }

    if (field.type == AppConfigFieldType::Bool) {
        String raw;
        if (!raw_value(cfg, field, raw)) return false;
        out = raw == "1" ? "on" : "off";
        return true;
    }

    if (!raw_value(cfg, field, out)) return false;
    if (!out.length() && field.type == AppConfigFieldType::String) {
        set_empty_text(out);
    }
    return true;
}

bool app_config_field_set(AppConfig &config,
                          const AppConfigFieldDescriptor &field,
                          const String &value,
                          bool keep_secret_sentinel,
                          AppConfigFieldSetResult &result) {
    result = {};

    config.begin_update();
    const bool ok = app_config_field_set_in_update(
        config, field, value, keep_secret_sentinel, result);
    if (!ok) {
        config.commit_update();
        return false;
    }

    const bool committed = config.commit_update();
    if (!committed) return false;
    return true;
}

bool app_config_field_set_in_update(AppConfig &config,
                                    const AppConfigFieldDescriptor &field,
                                    const String &value,
                                    bool keep_secret_sentinel,
                                    AppConfigFieldSetResult &result) {
    result = {};

    String before;
    if (!raw_value(config.data(), field, before)) return false;

    const bool ok = set_field_value(config, field, value,
                                    keep_secret_sentinel);
    if (!ok) return false;

    String after;
    if (!raw_value(config.data(), field, after)) return false;

    result.accepted = true;
    result.changed = before != after;
    result.dirty = result.changed ? field.dirty : 0;
    return true;
}
#endif

}  // namespace aircannect
