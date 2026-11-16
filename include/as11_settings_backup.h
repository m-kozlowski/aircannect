#pragma once

#include "as11_settings.h"
#include "large_allocator.h"
#include "large_byte_buffer.h"
#include "settings_backup_preview.h"

namespace aircannect {

class As11DeviceState;

struct As11SettingsBackupPlan {
    As11PreparedSettingsWrite write;
    BackupSettingChanges entries;
    std::string source_serial;
    std::string source_name;
    std::string created;
    size_t unchanged = 0;
    int mode = -1;
};

// Files contain the same values/units accepted by the settings write owner.
std::shared_ptr<const LargeByteBuffer> as11_settings_backup_encode(
    const As11SettingsState &state, const As11DeviceState &device,
    const char *created);

// Validate the external file once and retain the exact prepared write shown
// in the preview. The caller checks device/settings revisions before apply.
const char *as11_settings_backup_prepare(
    const LargeByteBuffer &file, const As11SettingsState &state,
    As11SettingsBackupPlan &plan);

}  // namespace aircannect
