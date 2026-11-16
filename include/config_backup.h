#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "config_backup_format.h"
#include "large_allocator.h"
#include "large_text_buffer.h"

namespace aircannect {

class BleSensorSource;
class ConfigService;
class WifiManager;

struct ConfigRestoreIssue {
    std::string key;
    const char *reason = "";
};

struct ConfigRestoreResult {
    size_t applied = 0;
    size_t kept = 0;
    size_t rejected = 0;
    std::vector<ConfigRestoreIssue, LargeAllocator<ConfigRestoreIssue>> issues;

    bool config_persisted = false;
    bool wifi_saved = true;
    bool sensors_saved = true;
    uint32_t config_revision = 0;
    const char *error = nullptr;

    bool ok() const {
        return config_persisted && wifi_saved && sensors_saved && !error;
    }
};

// One restore of backup text. SD provisioning and the WebUI submit lines;
// each owner applies its part only in finish(): one ConfigService
// transaction, one Wi-Fi profile replacement and one saved-oximeter list.
class ConfigRestore {
public:
    static constexpr size_t MaxIssues = 32;

    ConfigRestore(ConfigService &config,
                  WifiManager &wifi,
                  BleSensorSource *sensors);

    void add_line(const std::string &line, bool too_long = false);
    void add_text(const char *text, size_t length);
    ConfigRestoreResult finish();

private:
    struct PendingConfig {
        std::string key;
        std::string value;
        const AppConfigFieldDescriptor *field = nullptr;
    };

    struct PendingWifi {
        bool ssid_set = false;
        bool password_set = false;
        std::string ssid;
        std::string password;
    };

    struct PendingSensor {
        bool set = false;
        ConfigBackupSensor sensor;
    };

    // config transaction
    void apply_config(ConfigRestoreResult &result);
    bool apply_config_pass(bool skip_vpn, ConfigRestoreResult &pass);
    void apply_config_entry(const PendingConfig &entry,
                            ConfigRestoreResult &pass);

    // Wi-Fi profiles and saved oximeters
    void apply_wifi(ConfigRestoreResult &result);
    void apply_sensors(ConfigRestoreResult &result);

    static void reject(ConfigRestoreResult &result,
                       const std::string &key,
                       const char *reason);

    ConfigService &config_;
    WifiManager &wifi_;
    BleSensorSource *sensors_ = nullptr;

    ConfigRestoreResult parsed_;
    size_t line_number_ = 0;

    std::vector<PendingConfig, LargeAllocator<PendingConfig>> config_entries_;

    PendingWifi wifi_entries_[AC_WIFI_PROFILE_MAX];
    bool wifi_touched_ = false;

    PendingSensor sensor_entries_[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    bool sensors_touched_ = false;
};

// Owns configuration backup text for every persisted setting owner.
// Called from the main loop, which also owns those settings.
class ConfigBackup {
public:
    void begin(ConfigService &config,
               WifiManager &wifi,
               BleSensorSource *sensors);
    bool ready() const { return config_ && wifi_; }

    bool export_text(const ConfigBackupOptions &options,
                     LargeTextBuffer &out) const;
    ConfigRestoreResult restore_text(const char *text, size_t length);

private:
    void export_config(const ConfigBackupOptions &options,
                       LargeTextBuffer &out) const;
    void export_wifi(const ConfigBackupOptions &options,
                     LargeTextBuffer &out) const;
    void export_sensors(LargeTextBuffer &out) const;

    ConfigService *config_ = nullptr;
    WifiManager *wifi_ = nullptr;
    BleSensorSource *sensors_ = nullptr;
};

}  // namespace aircannect
