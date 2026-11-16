#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "app_config_registry.h"
#include "board_net.h"
#include "board_oximetry.h"

namespace aircannect {

class LargeTextBuffer;

// Text format shared by SD provisioning (config.txt) and WebUI configuration
// backups: one "key=value" entry per line. Lines starting with '#' or ';'
// are comments. Keys are exact registry keys, Wi-Fi profile keys ssid_N /
// pass_N, and saved oximeter keys oxi_sensor_N.

// A secret written as this marker keeps the value saved on the device.
static constexpr const char *AC_CONFIG_BACKUP_KEEP_MARKER =
    AC_CONFIG_SECRET_SENTINEL;
static constexpr const char *AC_CONFIG_BACKUP_SENSOR_PREFIX = "oxi_sensor_";

// line parsing
enum class ConfigBackupLineKind : uint8_t {
    Ignored,
    Entry,
    Invalid,
};

struct ConfigBackupEntry {
    std::string key;
    std::string value;
};

ConfigBackupLineKind config_backup_parse_line(const std::string &line,
                                              ConfigBackupEntry &entry);

// key classification
enum class ConfigBackupKeyKind : uint8_t {
    Unknown,
    Config,
    WifiSsid,
    WifiPassword,
    OximetrySensor,
};

struct ConfigBackupKey {
    ConfigBackupKeyKind kind = ConfigBackupKeyKind::Unknown;
    size_t index = 0;
    const AppConfigFieldDescriptor *field = nullptr;
};

ConfigBackupKey config_backup_classify_key(const std::string &key);

// export selection
struct ConfigBackupOptions {
    bool secrets = false;
    bool internal = false;
};

enum class ConfigBackupFieldExport : uint8_t {
    Skip,
    Value,
    KeepMarker,
};

// Internal fields (AS11 BLE pairing) are exported as one complete unit,
// including their key, only when requested and set: restoring an empty
// pairing would remove the pairing of the receiving device. A secret left
// out of the backup is written as the keep marker when it is set.
ConfigBackupFieldExport config_backup_field_export(
    const AppConfigFieldDescriptor &field,
    const ConfigBackupOptions &options,
    bool value_empty);

bool config_backup_is_keep_marker(const std::string &value);
bool config_backup_value_storable(const std::string &key,
                                  const std::string &value);
void config_backup_append_entry(LargeTextBuffer &out,
                                const std::string &key,
                                const std::string &value);

// saved oximeters: "address,address_type,on|off,name"
struct ConfigBackupSensor {
    std::string address;
    uint8_t address_type = 1;
    bool autoconnect = true;
    std::string name;
};

std::string config_backup_format_sensor(const ConfigBackupSensor &sensor);
bool config_backup_parse_sensor(const std::string &value,
                                ConfigBackupSensor &sensor);

// keybindings travel as the hex text of their persisted blob
bool config_backup_decode_hex(const std::string &text,
                              std::vector<uint8_t> &bytes);

}  // namespace aircannect
