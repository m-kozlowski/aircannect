#pragma once

#include <stdint.h>
#include <string>

#include "edf_str_session.h"

namespace aircannect {

struct EdfStrSettingsApplyResult {
    bool ok = false;
    const char *error = nullptr;
    uint32_t values = 0;
    uint32_t missing = 0;
    uint32_t unmapped = 0;
};

std::string edf_str_setting_get_names();

bool edf_str_apply_settings_response(const std::string &payload,
                                     EdfStrSessionAccumulator &session,
                                     EdfStrSettingsApplyResult &result);

}  // namespace aircannect
