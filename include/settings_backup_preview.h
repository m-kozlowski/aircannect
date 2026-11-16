#pragma once

#include <string>
#include <vector>

#include "large_allocator.h"

namespace aircannect {

struct BackupSettingChange {
    std::string key;
    std::string label;
    std::string before;
    std::string value;
    const char *reason = nullptr;
    int before_option = -1;
    int value_option = -1;
    bool stock = false;
    bool unchanged = false;
    bool secret = false;
    bool kept = false;
};

using BackupSettingChanges =
    std::vector<BackupSettingChange, LargeAllocator<BackupSettingChange>>;

}  // namespace aircannect
