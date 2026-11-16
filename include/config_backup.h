#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "config_backup_format.h"
#include "config_service.h"
#include "large_allocator.h"
#include "large_text_buffer.h"
#include "oximetry_status.h"
#include "settings_backup_preview.h"
#include "wifi_manager.h"

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
// prepare() resolves the accepted values without writing. finish() applies
// that same candidate only while the previewed current settings still match.
class ConfigRestore {
public:
    static constexpr size_t MaxIssues = 32;

    ConfigRestore(ConfigService &config,
                  WifiManager &wifi,
                  BleSensorSource *sensors);

    void add_line(const std::string &line, bool too_long = false);
    void add_text(const char *text, size_t length);
    const ConfigRestoreResult &prepare();
    const BackupSettingChanges &entries() const { return entries_; }
    ConfigRestoreResult finish();

private:
    struct PendingConfig {
        std::string key;
        std::string value;
        const AppConfigFieldDescriptor *field = nullptr;
        bool accepted = false;
        bool kept = false;
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
    void prepare_config(ConfigRestoreResult &result);
    bool prepare_config_pass(bool skip_vpn, ConfigRestoreResult &pass);
    void prepare_config_entry(PendingConfig &entry,
                            ConfigRestoreResult &pass);
    void describe_config();

    // Wi-Fi profiles and saved oximeters
    void prepare_wifi(ConfigRestoreResult &result);
    void prepare_sensors(ConfigRestoreResult &result);
    bool lists_unchanged() const;

    static void reject(ConfigRestoreResult &result,
                       const std::string &key,
                       const char *reason);

    ConfigService &config_;
    WifiManager &wifi_;
    BleSensorSource *sensors_ = nullptr;

    ConfigRestoreResult parsed_;
    size_t line_number_ = 0;
    bool prepared_ = false;
    ConfigService::PreparedUpdate config_update_;
    BackupSettingChanges entries_;

    std::vector<PendingConfig, LargeAllocator<PendingConfig>> config_entries_;

    PendingWifi wifi_entries_[AC_WIFI_PROFILE_MAX];
    bool wifi_touched_ = false;
    std::vector<WifiProfile, LargeAllocator<WifiProfile>> wifi_before_, wifi_after_;
    bool wifi_changed_ = false;

    PendingSensor sensor_entries_[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    bool sensors_touched_ = false;
    std::vector<OximetrySensorDevice, LargeAllocator<OximetrySensorDevice>>
        sensors_before_, sensors_after_;
    bool sensors_changed_ = false;
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
    std::shared_ptr<ConfigRestore> prepare_restore(const char *text, size_t length);

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
