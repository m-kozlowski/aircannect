#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "board_net.h"

namespace aircannect {

class ArduinoOtaSource;
class FirmwareInstaller;
class FirmwareUrlSource;
class UpdateChecker;

struct OtaStatusSnapshot {
    bool arduino_started = false;
    bool arduino_active = false;
    bool http_prepare_pending = false;
    bool http_prepared = false;
    bool http_active = false;
    bool http_ready = false;
    bool url_active = false;
    bool update_check_enabled = false;
    bool update_check_pending = false;
    bool update_check_active = false;
    bool update_check_attempted = false;
    bool update_checked = false;
    bool update_available = false;
    bool update_installable = false;
    bool reboot_pending = false;
    bool auth_enabled = true;
    uint16_t arduino_port = AC_ARDUINO_OTA_PORT;
    size_t bytes = 0;
    size_t total_size = 0;
    size_t wire_bytes = 0;
    size_t wire_total_size = 0;
    uint8_t progress_percent = 0;
    String method = "idle";
    String encoding = "auto";
    String partition;
    String last_error;
    String update_version;
    String update_error;
    uint32_t update_last_check_age_ms = 0;
};

OtaStatusSnapshot collect_ota_status(
    const FirmwareInstaller &installer,
    const FirmwareUrlSource &url_source,
    const ArduinoOtaSource &arduino_source,
    const UpdateChecker &update_checker);

bool request_selected_update(UpdateChecker &update_checker,
                             FirmwareUrlSource &url_source,
                             String &error);

void abort_esp_ota(FirmwareInstaller &installer,
                   FirmwareUrlSource &url_source,
                   ArduinoOtaSource &arduino_source,
                   UpdateChecker &update_checker,
                   const char *reason);

}  // namespace aircannect
