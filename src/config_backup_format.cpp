#include "config_backup_format.h"
#include "large_text_buffer.h"

#include <ctype.h>
#include <string.h>

#include "board_storage.h"

namespace aircannect {
namespace {

std::string trimmed(const std::string &text) {
    size_t first = 0;
    while (first < text.size() &&
           isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }

    size_t last = text.size();
    while (last > first &&
           isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return text.substr(first, last - first);
}

bool starts_with(const std::string &text, const char *prefix) {
    return text.compare(0, strlen(prefix), prefix) == 0;
}

bool parse_index(const std::string &text, size_t limit, size_t &index) {
    if (text.empty() || text.size() > 3) return false;

    size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<size_t>(c - '0');
    }
    if (value >= limit) return false;

    index = value;
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool valid_sensor_address(const std::string &address) {
    if (address.size() != 17) return false;

    for (size_t i = 0; i < address.size(); ++i) {
        if (i % 3 == 2) {
            if (address[i] != ':') return false;
        } else if (hex_digit(address[i]) < 0) {
            return false;
        }
    }
    return true;
}

bool printable_text(const std::string &text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return false;
    }
    return true;
}

}  // namespace

ConfigBackupLineKind config_backup_parse_line(const std::string &line,
                                              ConfigBackupEntry &entry) {
    entry = {};

    std::string text = line;
    while (!text.empty() && text.back() == '\r') text.pop_back();

    const std::string control = trimmed(text);
    if (control.empty() || control[0] == '#' || control[0] == ';') {
        return ConfigBackupLineKind::Ignored;
    }

    const size_t separator = text.find('=');
    if (separator == std::string::npos || separator == 0) {
        return ConfigBackupLineKind::Invalid;
    }

    entry.key = trimmed(text.substr(0, separator));
    if (entry.key.empty()) return ConfigBackupLineKind::Invalid;

    entry.value = text.substr(separator + 1);
    return ConfigBackupLineKind::Entry;
}

ConfigBackupKey config_backup_classify_key(const std::string &key) {
    ConfigBackupKey out;

    if (starts_with(key, "ssid_") &&
        parse_index(key.substr(5), AC_WIFI_PROFILE_MAX, out.index)) {
        out.kind = ConfigBackupKeyKind::WifiSsid;
        return out;
    }
    if (starts_with(key, "pass_") &&
        parse_index(key.substr(5), AC_WIFI_PROFILE_MAX, out.index)) {
        out.kind = ConfigBackupKeyKind::WifiPassword;
        return out;
    }

    const size_t sensor_prefix = strlen(AC_CONFIG_BACKUP_SENSOR_PREFIX);
    if (starts_with(key, AC_CONFIG_BACKUP_SENSOR_PREFIX) &&
        parse_index(key.substr(sensor_prefix),
                    AC_OXIMETRY_SENSOR_MAX_KNOWN, out.index)) {
        out.kind = ConfigBackupKeyKind::OximetrySensor;
        return out;
    }

    out.index = 0;
    out.field = app_config_find_field(key.c_str());
    if (out.field) out.kind = ConfigBackupKeyKind::Config;
    return out;
}

ConfigBackupFieldExport config_backup_field_export(
    const AppConfigFieldDescriptor &field,
    const ConfigBackupOptions &options,
    bool value_empty) {
    if (!field.available) return ConfigBackupFieldExport::Skip;

    if ((field.flags & AC_CONFIG_FIELD_INTERNAL) != 0) {
        return options.internal && !value_empty
            ? ConfigBackupFieldExport::Value
            : ConfigBackupFieldExport::Skip;
    }

    if (app_config_field_is_secret(field) && !options.secrets) {
        return value_empty ? ConfigBackupFieldExport::Skip
                           : ConfigBackupFieldExport::KeepMarker;
    }
    return ConfigBackupFieldExport::Value;
}

bool config_backup_is_keep_marker(const std::string &value) {
    return value == AC_CONFIG_BACKUP_KEEP_MARKER;
}

bool config_backup_value_storable(const std::string &key,
                                  const std::string &value) {
    if (key.size() + 1 + value.size() > AC_PROVISION_LINE_MAX) return false;

    for (const char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

void config_backup_append_entry(LargeTextBuffer &out,
                                const std::string &key,
                                const std::string &value) {
    out += key.c_str();
    out += '=';
    out += value.c_str();
    out += '\n';
}

std::string config_backup_format_sensor(const ConfigBackupSensor &sensor) {
    std::string out = sensor.address;
    out += ',';
    out += std::to_string(static_cast<unsigned>(sensor.address_type));
    out += ',';
    out += sensor.autoconnect ? "on" : "off";
    if (sensor.recordings) out += "+recordings";
    out += ',';
    out += sensor.name;
    return out;
}

bool config_backup_parse_sensor(const std::string &value,
                                ConfigBackupSensor &sensor) {
    sensor = {};

    const size_t first = value.find(',');
    const size_t second =
        first == std::string::npos ? first : value.find(',', first + 1);
    const size_t third =
        second == std::string::npos ? second : value.find(',', second + 1);
    if (third == std::string::npos) return false;

    sensor.address = trimmed(value.substr(0, first));
    for (char &c : sensor.address) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (!valid_sensor_address(sensor.address)) return false;

    const std::string type = trimmed(value.substr(first + 1,
                                                  second - first - 1));
    if (type.size() != 1 || type[0] < '0' || type[0] > '3') return false;
    sensor.address_type = static_cast<uint8_t>(type[0] - '0');

    std::string autoconnect =
        trimmed(value.substr(second + 1, third - second - 1));
    const auto option = autoconnect.find('+');
    if (option != std::string::npos) {
        if (autoconnect.substr(option) != "+recordings") return false;
        sensor.recordings = true;
        autoconnect.resize(option);
    }
    if (autoconnect == "on" || autoconnect == "1") {
        sensor.autoconnect = true;
    } else if (autoconnect == "off" || autoconnect == "0") {
        sensor.autoconnect = false;
    } else {
        return false;
    }

    sensor.name = value.substr(third + 1);
    return sensor.name.size() <= AC_OXIMETRY_SENSOR_NAME_MAX &&
           printable_text(sensor.name);
}

bool config_backup_decode_hex(const std::string &text,
                              std::vector<uint8_t> &bytes) {
    bytes.clear();
    const std::string hex = trimmed(text);
    if (hex.empty() || hex.size() % 2 != 0) return false;

    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_digit(hex[i]);
        const int low = hex_digit(hex[i + 1]);
        if (high < 0 || low < 0) {
            bytes.clear();
            return false;
        }
        bytes.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

}  // namespace aircannect
