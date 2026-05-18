#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#include "app_config.h"

namespace aircannect {

bool network_auth_required(const AppConfigData &cfg);
uint32_t ip_to_u32(const IPAddress &ip);
bool auth_whitelist_matches(const IPAddress &remote_ip,
                            const String &whitelist);
bool network_client_allowed(const AppConfigData &cfg,
                            const IPAddress &remote_ip);
bool network_credentials_match(const AppConfigData &cfg,
                               const String &user,
                               const String &password);

}  // namespace aircannect
