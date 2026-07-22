#include "export_endpoint_config.h"

#include <string.h>

#include "app_config.h"
#include "string_util.h"

namespace aircannect {

ExportEndpointConfig make_export_endpoint_config(
    const AppConfigData &config) {
    ExportEndpointConfig out;

    out.smb.enabled = config.smb_endpoint.length() != 0;
    copy_cstr(out.smb.endpoint,
              sizeof(out.smb.endpoint),
              config.smb_endpoint.c_str());
    copy_cstr(out.smb.user,
              sizeof(out.smb.user),
              config.smb_user.c_str());
    copy_cstr(out.smb.password,
              sizeof(out.smb.password),
              config.smb_password.c_str());

    copy_cstr(out.sleephq.client_id,
              sizeof(out.sleephq.client_id),
              config.sleephq_client_id.c_str());
    copy_cstr(out.sleephq.client_secret,
              sizeof(out.sleephq.client_secret),
              config.sleephq_client_secret.c_str());
    copy_cstr(out.sleephq.team_id,
              sizeof(out.sleephq.team_id),
              config.sleephq_team_id.c_str());
    copy_cstr(out.sleephq.device_id,
              sizeof(out.sleephq.device_id),
              config.sleephq_device_id.c_str());
    return out;
}

bool export_endpoint_config_equal(const ExportEndpointConfig &left,
                                  const ExportEndpointConfig &right) {
    return left.smb.enabled == right.smb.enabled &&
           strcmp(left.smb.endpoint, right.smb.endpoint) == 0 &&
           strcmp(left.smb.user, right.smb.user) == 0 &&
           strcmp(left.smb.password, right.smb.password) == 0 &&
           strcmp(left.sleephq.client_id, right.sleephq.client_id) == 0 &&
           strcmp(left.sleephq.client_secret,
                  right.sleephq.client_secret) == 0 &&
           strcmp(left.sleephq.team_id, right.sleephq.team_id) == 0 &&
           strcmp(left.sleephq.device_id, right.sleephq.device_id) == 0;
}

}  // namespace aircannect
