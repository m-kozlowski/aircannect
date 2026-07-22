#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "app_config.h"
#include "firmware_installer.h"
#include "runtime_snapshots.h"

class ArduinoOTAClass;

namespace aircannect {

struct ArduinoOtaSourceStatus {
    bool started = false;
    bool active = false;
    bool auth_enabled = true;
    uint16_t port = 0;
    size_t bytes = 0;
    size_t total_size = 0;
    uint8_t progress_percent = 0;
    String last_error;
};

class ArduinoOtaSource {
public:
    explicit ArduinoOtaSource(FirmwareInstaller &installer)
        : installer_(installer) {}

    void begin(const AppConfigData &config);
    void poll(const NetworkSnapshot &network,
              bool service_allowed,
              bool polling_allowed);
    void mark_config_dirty();
    void request_abort(const char *reason);

    bool active() const;
    ArduinoOtaSourceStatus status() const;

private:
    void start();
    void stop();
    void set_error(const char *error);
    bool lock(TickType_t timeout = portMAX_DELAY) const;
    void unlock() const;

    FirmwareInstaller &installer_;
    const AppConfigData *config_ = nullptr;
    ArduinoOTAClass *arduino_ota_ = nullptr;

    ArduinoOtaSourceStatus status_;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    bool config_dirty_ = true;
    uint32_t last_progress_log_percent_ = 255;
};

}  // namespace aircannect
