#pragma once

#include <stddef.h>

#include "sleephq_client.h"

namespace aircannect {

struct AppConfigData;

static constexpr size_t AC_SMB_EXPORT_ENDPOINT_MAX = 161;
static constexpr size_t AC_SMB_EXPORT_USER_MAX = 65;
static constexpr size_t AC_SMB_EXPORT_PASSWORD_MAX = 129;

struct SmbExportConfig {
    bool enabled = false;
    char endpoint[AC_SMB_EXPORT_ENDPOINT_MAX] = {};
    char user[AC_SMB_EXPORT_USER_MAX] = {};
    char password[AC_SMB_EXPORT_PASSWORD_MAX] = {};
};

struct SleepHqExportConfig {
    char client_id[AC_SLEEPHQ_SECRET_MAX] = {};
    char client_secret[AC_SLEEPHQ_SECRET_MAX] = {};
    char team_id[AC_SLEEPHQ_ID_MAX] = {};
    char device_id[AC_SLEEPHQ_ID_MAX] = {};
};

struct ExportEndpointConfig {
    SmbExportConfig smb;
    SleepHqExportConfig sleephq;
};

ExportEndpointConfig make_export_endpoint_config(const AppConfigData &config);
bool export_endpoint_config_equal(const ExportEndpointConfig &left,
                                  const ExportEndpointConfig &right);

}  // namespace aircannect
