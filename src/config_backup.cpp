#include "config_backup.h"

#include <string.h>
#include <algorithm>
#include <set>

#include "ble_sensor_source.h"
#include "board_storage.h"
#include "button_binding_config.h"
#include "config_service.h"
#include "debug_log.h"
#include "version.h"
#include "wifi_manager.h"

namespace aircannect {
namespace {

std::string index_key(const char *prefix, size_t index) {
    return std::string(prefix) + std::to_string(index);
}

bool same_ssid(const String &saved, const std::string &restored) {
    String left = saved;
    String right = restored.c_str();
    left.trim();
    right.trim();
    return left.length() > 0 && left == right;
}

bool same_sensor(const OximetrySensorDevice &a, const OximetrySensorDevice &b) {
    return strcmp(a.addr, b.addr) == 0 && strcmp(a.name, b.name) == 0 &&
           a.addr_type == b.addr_type && a.autoconnect == b.autoconnect;
}

std::string sensor_value(const OximetrySensorDevice &device) {
    ConfigBackupSensor sensor;
    sensor.address = device.addr;
    sensor.address_type = device.addr_type;
    sensor.name = device.name;
    sensor.autoconnect = device.autoconnect;
    return config_backup_format_sensor(sensor);
}

void append_comment(LargeTextBuffer &out, const char *text) {
    out += "# ";
    out += text;
    out += '\n';
}

void append_checked(LargeTextBuffer &out,
                    const std::string &key,
                    const std::string &value) {
    if (config_backup_value_storable(key, value)) {
        config_backup_append_entry(out, key, value);
        return;
    }

    out += "# ";
    out += key.c_str();
    out += ": value cannot be stored in this file\n";
}

}  // namespace

ConfigRestore::ConfigRestore(ConfigService &config,
                             WifiManager &wifi,
                             BleSensorSource *sensors)
    : config_(config), wifi_(wifi), sensors_(sensors) {}

void ConfigRestore::add_line(const std::string &line, bool too_long) {
    ++line_number_;
    if (too_long) {
        reject(parsed_, index_key("line ", line_number_), "line_too_long");
        return;
    }

    ConfigBackupEntry entry;
    const ConfigBackupLineKind kind = config_backup_parse_line(line, entry);
    if (kind == ConfigBackupLineKind::Ignored) return;
    if (kind == ConfigBackupLineKind::Invalid) {
        reject(parsed_, index_key("line ", line_number_), "invalid_line");
        return;
    }

    const ConfigBackupKey key = config_backup_classify_key(entry.key);
    switch (key.kind) {
        case ConfigBackupKeyKind::Config:
            config_entries_.push_back(
                {std::move(entry.key), std::move(entry.value), key.field});
            return;

        case ConfigBackupKeyKind::WifiSsid:
            wifi_touched_ = true;
            wifi_entries_[key.index].ssid_set = true;
            wifi_entries_[key.index].ssid = std::move(entry.value);
            return;

        case ConfigBackupKeyKind::WifiPassword:
            wifi_entries_[key.index].password_set = true;
            wifi_entries_[key.index].password = std::move(entry.value);
            return;

        case ConfigBackupKeyKind::OximetrySensor: {
            ConfigBackupSensor sensor;
            if (!config_backup_parse_sensor(entry.value, sensor)) {
                reject(parsed_, entry.key, "invalid_value");
                return;
            }
            sensors_touched_ = true;
            sensor_entries_[key.index].set = true;
            sensor_entries_[key.index].sensor = std::move(sensor);
            return;
        }

        case ConfigBackupKeyKind::Unknown:
            reject(parsed_, entry.key, "unknown_key");
            return;
    }
}

void ConfigRestore::add_text(const char *text, size_t length) {
    if (!text) return;

    size_t start = 0;
    while (start < length) {
        const char *newline = static_cast<const char *>(
            memchr(text + start, '\n', length - start));
        const size_t end = newline ? static_cast<size_t>(newline - text)
                                   : length;
        const size_t line_length = end - start;

        if (line_length > AC_PROVISION_LINE_MAX) {
            add_line(std::string(), true);
        } else {
            add_line(std::string(text + start, line_length));
        }
        start = end + 1;
    }
}

ConfigRestoreResult ConfigRestore::finish() {
    prepare();
    ConfigRestoreResult result = parsed_;
    result.applied = 0;
    if (result.error) return result;

    if (config_.revision() != result.config_revision || !lists_unchanged()) {
        result.error = "config_changed";
        return result;
    }

    if (config_update_.valid()) {
        const auto committed = config_.commit_prepared(std::move(config_update_));
        result.config_persisted = committed.persisted;
        result.config_revision = committed.revision;
        result.error = committed.error;
        if (!result.config_persisted || result.error) return result;
    } else {
        result.config_persisted = true;
    }
    result.applied = parsed_.applied;

    if (wifi_changed_) {
        result.wifi_saved = wifi_.replace_profiles(wifi_after_.data(), wifi_after_.size(), false);
        if (result.wifi_saved) result.applied += wifi_after_.size();
    }
    if (sensors_changed_) {
        result.sensors_saved = sensors_->replace_known_sensors(sensors_after_.data(), sensors_after_.size());
        if (result.sensors_saved) result.applied += sensors_after_.size();
    }
    return result;
}

const ConfigRestoreResult &ConfigRestore::prepare() {
    if (prepared_) return parsed_;
    prepared_ = true;

    prepare_config(parsed_);
    if (parsed_.error) return parsed_;

    describe_config();
    prepare_wifi(parsed_);
    prepare_sensors(parsed_);
    for (const auto &issue : parsed_.issues) {
        BackupSettingChange row;
        row.key = issue.key;
        row.reason = issue.reason;
        entries_.push_back(std::move(row));
    }
    return parsed_;
}

void ConfigRestore::prepare_config(ConfigRestoreResult &result) {
    result.config_revision = config_.revision();
    if (config_entries_.empty()) {
        return;
    }

    bool has_vpn = false;
    for (const PendingConfig &entry : config_entries_) {
        has_vpn = has_vpn || entry.field->group == AppConfigGroup::Vpn;
    }

    // WireGuard is validated as a whole when the candidate is prepared. If its
    // restored fields are incomplete, keep the current VPN and apply the rest.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool skip_vpn = attempt == 1;
        ConfigRestoreResult pass;
        if (!prepare_config_pass(skip_vpn, pass)) {
            result.error = pass.error;
            result.config_revision = config_.revision();
            return;
        }

        if (config_update_.valid()) {
            result.applied += pass.applied;
            result.kept += pass.kept;
            result.rejected += pass.rejected;
            for (ConfigRestoreIssue &issue : pass.issues) {
                if (result.issues.size() >= MaxIssues) break;
                result.issues.push_back(std::move(issue));
            }
            result.config_revision = pass.config_revision;
            return;
        }

        const bool retry = !skip_vpn && has_vpn && pass.error &&
                           strcmp(pass.error, "persist_failed") != 0;
        if (!retry) {
            result.error = pass.error ? pass.error : "config_not_saved";
            result.config_revision = config_.revision();
            return;
        }
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "config restore keeps current WireGuard reason=%s\n",
                  pass.error);
    }
}

bool ConfigRestore::prepare_config_pass(bool skip_vpn,
                                      ConfigRestoreResult &pass) {
    if (!config_.begin_transaction()) {
        pass.error = "transaction_busy";
        return false;
    }

    try {
        for (PendingConfig &entry : config_entries_) {
            entry.accepted = false;
            entry.kept = false;
            if (skip_vpn && entry.field->group == AppConfigGroup::Vpn) {
                reject(pass, entry.key, "wireguard_rejected");
                continue;
            }
            prepare_config_entry(entry, pass);
        }
        config_update_ = config_.prepare_transaction();
    } catch (...) {
        config_.cancel_transaction();
        throw;
    }

    pass.config_revision = config_update_.result().revision;
    pass.error = config_update_.result().error;
    return true;
}

void ConfigRestore::prepare_config_entry(PendingConfig &entry,
                                       ConfigRestoreResult &pass) {
    const AppConfigFieldDescriptor &field = *entry.field;
    if (app_config_field_is_secret(field) &&
        config_backup_is_keep_marker(entry.value)) {
        pass.kept++;
        entry.accepted = true;
        entry.kept = true;
        return;
    }

    if (field.type == AppConfigFieldType::Keybindings) {
        std::vector<uint8_t> bytes;
        ButtonBindingConfig keybindings;
        if (!config_backup_decode_hex(entry.value, bytes) ||
            !button_binding_config_decode(bytes.data(), bytes.size(),
                                          keybindings) ||
            !config_.set_transaction_keybindings(keybindings).accepted()) {
            reject(pass, entry.key, "invalid_value");
            return;
        }
        pass.applied++;
        entry.accepted = true;
        return;
    }

    const ConfigFieldUpdate update = config_.set_transaction_value(
        field.key, String(entry.value.c_str()), false);
    if (!update.accepted()) {
        reject(pass, entry.key, "invalid_value");
        return;
    }
    pass.applied++;
    entry.accepted = true;
}

void ConfigRestore::describe_config() {
    if (!config_update_.valid()) return;

    std::set<const AppConfigFieldDescriptor *> described;
    for (auto it = config_entries_.rbegin(); it != config_entries_.rend(); ++it) {
        if (!it->accepted || !described.insert(it->field).second) continue;

        const auto &field = *it->field;
        String before, after;
        app_config_field_get_raw_value(config_.data(), field, before);
        app_config_field_get_raw_value(config_update_.data(), field, after);
        BackupSettingChange row;
        row.key = field.key;
        row.label = field.label;
        row.unchanged = before == after;
        row.secret = app_config_field_is_secret(field);
        row.kept = it->kept && row.unchanged;
        if (row.secret) {
            row.before = before.length() ? "set" : "";
            row.value = after.length() ? "set" : "";
        } else {
            const bool before_empty = before.isEmpty();
            const bool after_empty = after.isEmpty();
            app_config_field_get_console_value(config_.data(), field, before);
            app_config_field_get_console_value(config_update_.data(), field, after);
            row.before = before_empty ? "" : before.c_str();
            row.value = after_empty ? "" : after.c_str();
        }
        entries_.push_back(std::move(row));
    }
    std::reverse(entries_.begin(), entries_.end());
}

void ConfigRestore::prepare_wifi(ConfigRestoreResult &result) {
    if (!wifi_touched_) return;

    for (size_t i = 0; i < wifi_.profile_count(); ++i) wifi_before_.push_back(wifi_.profile(i));

    bool kept[AC_WIFI_PROFILE_MAX] = {};
    size_t missing_passwords = 0;
    for (size_t i = 0; i < AC_WIFI_PROFILE_MAX; ++i) {
        const PendingWifi &entry = wifi_entries_[i];
        if (!entry.ssid_set) {
            if (entry.password_set) {
                reject(result, index_key("pass_", i), "missing_ssid");
            }
            continue;
        }

        // An empty SSID removes that slot, as documented for config.txt.
        String ssid = entry.ssid.c_str();
        ssid.trim();
        if (!ssid.length()) continue;

        String password = entry.password.c_str();
        if (entry.password_set &&
            config_backup_is_keep_marker(entry.password)) {
            bool found = false;
            for (size_t saved = 0; saved < wifi_.profile_count(); ++saved) {
                if (!same_ssid(wifi_.profile(saved).ssid, entry.ssid)) {
                    continue;
                }
                password = wifi_.profile(saved).password;
                found = true;
                break;
            }
            if (!found) {
                reject(result, index_key("pass_", i), "password_not_saved");
                missing_passwords++;
                continue;
            }
            result.kept++;
        }

        kept[wifi_after_.size()] = config_backup_is_keep_marker(entry.password);
        wifi_after_.push_back({ssid, password});
    }

    // A backup whose profiles all need passwords missing on this device
    // leaves the current profiles in place instead of removing them.
    if (wifi_after_.empty() && missing_passwords > 0) return;

    const WifiProfile empty;
    for (size_t i = 0; i < std::max(wifi_before_.size(), wifi_after_.size()); ++i) {
        const auto &before = i < wifi_before_.size() ? wifi_before_[i] : empty;
        const auto &after = i < wifi_after_.size() ? wifi_after_[i] : empty;
        for (bool password : {false, true}) {
            BackupSettingChange row;
            row.key = index_key(password ? "pass_" : "ssid_", i);
            row.label = std::string("Wi-Fi ") + std::to_string(i + 1) +
                        (password ? " password" : " SSID");
            row.secret = password;
            row.unchanged = password ? before.password == after.password : before.ssid == after.ssid;
            row.kept = password && kept[i] && row.unchanged;
            row.before = password ? (before.password.length() ? "set" : "") : before.ssid.c_str();
            row.value = password ? (after.password.length() ? "set" : "") : after.ssid.c_str();
            wifi_changed_ = wifi_changed_ || !row.unchanged;
            entries_.push_back(std::move(row));
        }
    }
}

void ConfigRestore::prepare_sensors(ConfigRestoreResult &result) {
    if (!sensors_touched_) return;

    if (sensors_) {
        sensors_before_.resize(AC_OXIMETRY_SENSOR_MAX_KNOWN);
        sensors_before_.resize(sensors_->known_sensors(sensors_before_.data(), sensors_before_.size()));
    }
    for (size_t i = 0; i < AC_OXIMETRY_SENSOR_MAX_KNOWN; ++i) {
        const PendingSensor &entry = sensor_entries_[i];
        if (!entry.set) continue;

        if (!sensors_) {
            reject(result,
                   index_key(AC_CONFIG_BACKUP_SENSOR_PREFIX, i),
                   "unsupported");
            continue;
        }

        if (std::any_of(sensors_after_.begin(), sensors_after_.end(), [&](const auto &device) {
                return strcasecmp(entry.sensor.address.c_str(), device.addr) == 0;
            })) continue;

        OximetrySensorDevice device;
        strncpy(device.addr, entry.sensor.address.c_str(),
                sizeof(device.addr) - 1);
        device.addr_type = entry.sensor.address_type;
        device.autoconnect = entry.sensor.autoconnect;
        strncpy(device.name, entry.sensor.name.c_str(),
                sizeof(device.name) - 1);
        sensors_after_.push_back(device);
    }
    if (!sensors_ || sensors_after_.empty()) return;

    for (size_t i = 0; i < std::max(sensors_before_.size(), sensors_after_.size()); ++i) {
        BackupSettingChange row;
        row.key = index_key(AC_CONFIG_BACKUP_SENSOR_PREFIX, i);
        row.label = "Oximeter " + std::to_string(i + 1);
        row.before = i < sensors_before_.size() ? sensor_value(sensors_before_[i]) : "";
        row.value = i < sensors_after_.size() ? sensor_value(sensors_after_[i]) : "";
        row.unchanged = row.before == row.value;
        sensors_changed_ = sensors_changed_ || !row.unchanged;
        entries_.push_back(std::move(row));
    }
}

bool ConfigRestore::lists_unchanged() const {
    if (wifi_touched_) {
        if (wifi_.profile_count() != wifi_before_.size()) return false;
        for (size_t i = 0; i < wifi_before_.size(); ++i) {
            if (wifi_.profile(i).ssid != wifi_before_[i].ssid ||
                wifi_.profile(i).password != wifi_before_[i].password) return false;
        }
    }
    if (sensors_touched_ && sensors_) {
        OximetrySensorDevice current[AC_OXIMETRY_SENSOR_MAX_KNOWN];
        const size_t count = sensors_->known_sensors(current, AC_OXIMETRY_SENSOR_MAX_KNOWN);
        if (count != sensors_before_.size()) return false;
        for (size_t i = 0; i < count; ++i) {
            if (!same_sensor(current[i], sensors_before_[i])) return false;
        }
    }
    return true;
}

void ConfigRestore::reject(ConfigRestoreResult &result,
                           const std::string &key,
                           const char *reason) {
    result.rejected++;
    if (result.issues.size() >= MaxIssues) return;

    ConfigRestoreIssue issue;
    issue.key = key;
    issue.reason = reason;
    result.issues.push_back(std::move(issue));
}

void ConfigBackup::begin(ConfigService &config,
                         WifiManager &wifi,
                         BleSensorSource *sensors) {
    config_ = &config;
    wifi_ = &wifi;
    sensors_ = sensors;
}

bool ConfigBackup::export_text(const ConfigBackupOptions &options,
                               LargeTextBuffer &out) const {
    out.clear();
    if (!ready()) return false;

    append_comment(out, "AirCANnect configuration backup");
    out += "# firmware: ";
    out += aircannect_version();
    out += '\n';
    append_comment(out, "Restore it from the WebUI, or copy it to the SD "
                        "card root as config.txt.");
    append_comment(out, options.secrets
        ? "Passwords and keys: included as plain text."
        : "Passwords and keys: not included; ******** keeps the value "
          "saved on the device.");
    append_comment(out, options.internal
        ? "AS11 Bluetooth pairing: included."
        : "AS11 Bluetooth pairing: not included.");

    export_config(options, out);
    export_wifi(options, out);
    export_sensors(out);
    return !out.overflowed();
}

void ConfigBackup::export_config(const ConfigBackupOptions &options,
                                 LargeTextBuffer &out) const {
    const AppConfigData &data = config_->data();

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        String raw;
        if (!app_config_field_get_raw_value(data, field, raw)) continue;

        const ConfigBackupFieldExport policy =
            config_backup_field_export(field, options, raw.length() == 0);
        if (policy == ConfigBackupFieldExport::Skip) continue;

        append_checked(out, field.key,
                       policy == ConfigBackupFieldExport::KeepMarker
                           ? std::string(AC_CONFIG_BACKUP_KEEP_MARKER)
                           : std::string(raw.c_str()));
    }
}

void ConfigBackup::export_wifi(const ConfigBackupOptions &options,
                               LargeTextBuffer &out) const {
    for (size_t i = 0; i < wifi_->profile_count(); ++i) {
        const WifiProfile &profile = wifi_->profile(i);
        const std::string password = profile.password.c_str();

        append_checked(out, index_key("ssid_", i), profile.ssid.c_str());
        append_checked(out, index_key("pass_", i),
                       options.secrets || password.empty()
                           ? password
                           : std::string(AC_CONFIG_BACKUP_KEEP_MARKER));
    }
}

void ConfigBackup::export_sensors(LargeTextBuffer &out) const {
    if (!sensors_) return;

    OximetrySensorDevice devices[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    const size_t count =
        sensors_->known_sensors(devices, AC_OXIMETRY_SENSOR_MAX_KNOWN);
    for (size_t i = 0; i < count; ++i) {
        ConfigBackupSensor sensor;
        sensor.address = devices[i].addr;
        sensor.address_type = devices[i].addr_type;
        sensor.autoconnect = devices[i].autoconnect;
        sensor.name = devices[i].name;

        append_checked(out,
                       index_key(AC_CONFIG_BACKUP_SENSOR_PREFIX, i),
                       config_backup_format_sensor(sensor));
    }
}

ConfigRestoreResult ConfigBackup::restore_text(const char *text,
                                               size_t length) {
    if (!ready()) {
        ConfigRestoreResult result;
        result.error = "backup_unavailable";
        return result;
    }

    ConfigRestore restore(*config_, *wifi_, sensors_);
    restore.add_text(text, length);
    return restore.finish();
}

std::shared_ptr<ConfigRestore> ConfigBackup::prepare_restore(const char *text, size_t length) {
    if (!ready()) return {};

    auto plan = std::allocate_shared<ConfigRestore>(LargeAllocator<ConfigRestore>{},
                                                   *config_, *wifi_, sensors_);
    plan->add_text(text, length);
    plan->prepare();
    return plan;
}

}  // namespace aircannect
