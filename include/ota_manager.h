#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "app_config.h"
#include "wifi_manager.h"

class ArduinoOTAClass;

namespace aircannect {

struct OtaManagerStatus {
    bool arduino_started = false;
    bool http_active = false;
    bool http_ready = false;
    bool reboot_pending = false;
    bool auth_enabled = true;
    uint16_t arduino_port = AC_ARDUINO_OTA_PORT;
    size_t bytes = 0;
    size_t total_size = 0;
    uint8_t progress_percent = 0;
    String method = "idle";
    String partition;
    String last_error;
};

class OtaManager {
public:
    void begin(AppConfig &app_config);
    void poll(const WifiManager &wifi_manager, bool reboot_allowed = true);
    void mark_config_dirty();

    bool begin_http_upload(const String &filename, size_t image_size);
    bool write_http_upload(const uint8_t *data, size_t len);
    bool finish_http_upload();
    void abort_http_upload(const char *reason);

    void schedule_reboot(uint32_t delay_ms = 750);

    bool active() const;
    OtaManagerStatus status() const;

private:
    void start_arduino_ota();
    void stop_arduino_ota();

    void set_error(const char *error);
    void clear_http_state();
    bool lock_status(TickType_t timeout = portMAX_DELAY) const;
    void unlock_status() const;

    AppConfig *app_config_ = nullptr;
    ArduinoOTAClass *arduino_ota_ = nullptr;

    bool arduino_config_dirty_ = true;
    bool arduino_active_ = false;

    uint32_t reboot_at_ms_ = 0;
    bool reboot_wait_logged_ = false;
    uint32_t last_progress_log_percent_ = 255;

    esp_ota_handle_t http_handle_ = 0;
    const esp_partition_t *http_partition_ = nullptr;
    OtaManagerStatus status_;
    mutable SemaphoreHandle_t status_mutex_ = nullptr;
};

}  // namespace aircannect
