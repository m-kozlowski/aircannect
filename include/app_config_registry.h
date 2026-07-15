#pragma once

#include <stddef.h>
#include <stdint.h>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#include "app_config.h"
#define AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO 1
#else
#define AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO 0
#endif

namespace aircannect {

static constexpr const char *AC_CONFIG_SECRET_SENTINEL = "********";
static constexpr size_t AC_CONFIG_LOG_FIELD_COUNT = 15;

enum class AppConfigGroup : uint8_t {
    Device,
    Network,
    Access,
    Ota,
    Logging,
    Time,
    Oximetry,
    Smb,
    SleepHq,
};

enum class AppConfigFieldType : uint8_t {
    Bool,
    UInt16,
    String,
    Secret,
    Enum,
    LogLevel,
};

enum AppConfigFieldFlags : uint16_t {
    AC_CONFIG_FIELD_NONE = 0,
    AC_CONFIG_FIELD_SECRET = 1u << 0,
    AC_CONFIG_FIELD_PROVISIONABLE = 1u << 1,
};

enum AppConfigDirty : uint32_t {
    AC_CONFIG_DIRTY_HOSTNAME = 1UL << 0,
    AC_CONFIG_DIRTY_TCP = 1UL << 1,
    AC_CONFIG_DIRTY_SOFTAP = 1UL << 2,
    AC_CONFIG_DIRTY_WIFI_COUNTRY = 1UL << 3,
    AC_CONFIG_DIRTY_TIMEZONE = 1UL << 4,
    AC_CONFIG_DIRTY_RESMED_TIME = 1UL << 5,
    AC_CONFIG_DIRTY_HTTP_AUTH = 1UL << 6,
    AC_CONFIG_DIRTY_AUTH_WHITELIST = 1UL << 7,
    AC_CONFIG_DIRTY_TELNET = 1UL << 8,
    AC_CONFIG_DIRTY_OTA_PASSWORD = 1UL << 9,
    AC_CONFIG_DIRTY_OXIMETRY = 1UL << 10,
    AC_CONFIG_DIRTY_EDF_CAPTURE = 1UL << 11,
    AC_CONFIG_DIRTY_LOG_LEVELS = 1UL << 12,
    AC_CONFIG_DIRTY_SYSLOG = 1UL << 13,
    AC_CONFIG_DIRTY_SMB_SYNC = 1UL << 14,
    AC_CONFIG_DIRTY_SLEEPHQ_SYNC = 1UL << 15,
    AC_CONFIG_DIRTY_FILE_LOG = 1UL << 16,
    AC_CONFIG_DIRTY_UPDATE_URL = 1UL << 17,
};

static constexpr uint32_t AC_CONFIG_DIRTY_ALL =
    AC_CONFIG_DIRTY_HOSTNAME | AC_CONFIG_DIRTY_TCP |
    AC_CONFIG_DIRTY_SOFTAP | AC_CONFIG_DIRTY_WIFI_COUNTRY |
    AC_CONFIG_DIRTY_TIMEZONE | AC_CONFIG_DIRTY_RESMED_TIME |
    AC_CONFIG_DIRTY_HTTP_AUTH | AC_CONFIG_DIRTY_AUTH_WHITELIST |
    AC_CONFIG_DIRTY_TELNET | AC_CONFIG_DIRTY_OTA_PASSWORD |
    AC_CONFIG_DIRTY_OXIMETRY | AC_CONFIG_DIRTY_EDF_CAPTURE |
    AC_CONFIG_DIRTY_LOG_LEVELS | AC_CONFIG_DIRTY_SYSLOG |
    AC_CONFIG_DIRTY_SMB_SYNC | AC_CONFIG_DIRTY_SLEEPHQ_SYNC |
    AC_CONFIG_DIRTY_FILE_LOG | AC_CONFIG_DIRTY_UPDATE_URL;

enum class AppConfigFieldId : uint8_t {
    Hostname,
    TcpEnabled,
    TcpPort,
    SoftApMode,
    WifiCountry,
    Timezone,
    ResMedTimeSync,
    OximetryEnabled,
    OximetryUdpPort,
    OximetryAdvertiseMode,
    EdfCaptureEnabled,
    SmbEndpoint,
    SmbUser,
    SmbPassword,
    SleepHqClientId,
    SleepHqClientSecret,
    SleepHqTeamId,
    SleepHqDeviceId,
    HttpUser,
    HttpPassword,
    AuthWhitelist,
    TelnetEnabled,
    TelnetPort,
    OtaPassword,
    UpdateUrl,
    SyslogEnabled,
    SyslogHost,
    SyslogPort,
    FileLogEnabled,
    LogLevel,
};

struct AppConfigEnumValue {
    const char *value;
    const char *label;
};

struct AppConfigFieldDescriptor {
    const char *key;
    AppConfigFieldId id;
    AppConfigGroup group;
    uint16_t order;
    AppConfigFieldType type;
    uint16_t flags;
    uint32_t dirty;
    const char *label;
    const char *help;
    const AppConfigEnumValue *enum_values;
    size_t enum_value_count;
    int16_t index;
    size_t offset;
};

struct AppConfigFieldSetResult {
    bool accepted = false;
    bool changed = false;
    uint32_t dirty = 0;
};

const AppConfigFieldDescriptor *app_config_fields(size_t &count);
const AppConfigFieldDescriptor *app_config_find_field(const char *key);
const char *app_config_group_id(AppConfigGroup group);
const char *app_config_group_label(AppConfigGroup group);
bool app_config_field_is_secret(const AppConfigFieldDescriptor &field);
bool app_config_secret_sentinel_should_preserve(const char *current_value,
                                                const char *incoming_value,
                                                bool keep_secret_sentinel);

#if AIRCANNECT_CONFIG_REGISTRY_HAS_ARDUINO
bool app_config_field_get_console_value(const AppConfigData &cfg,
                                        const AppConfigFieldDescriptor &field,
                                        String &out);
bool app_config_field_get_raw_value(const AppConfigData &cfg,
                                    const AppConfigFieldDescriptor &field,
                                    String &out);
bool app_config_field_set(AppConfig &config,
                          const AppConfigFieldDescriptor &field,
                          const String &value,
                          bool keep_secret_sentinel,
                          AppConfigFieldSetResult &result);
bool app_config_field_set_in_update(AppConfig &config,
                                    const AppConfigFieldDescriptor &field,
                                    const String &value,
                                    bool keep_secret_sentinel,
                                    AppConfigFieldSetResult &result);
#endif

}  // namespace aircannect
