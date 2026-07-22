#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdint.h>

#include "firmware_installer.h"

namespace aircannect {

struct FirmwareUrlSourceStatus {
    bool active = false;
    String last_error;
};

class FirmwareUrlSource {
public:
    explicit FirmwareUrlSource(FirmwareInstaller &installer)
        : installer_(installer) {}

    void begin();
    bool request(const String &url,
                 OtaUploadEncoding encoding = OtaUploadEncoding::Auto,
                 size_t image_size = 0,
                 size_t wire_size = 0);
    void request_abort(const char *reason);

    bool active() const;
    FirmwareUrlSourceStatus status() const;

private:
    static void task_entry(void *ctx);
    void run_task();
    void finish_task(char *url);
    void fail(const char *reason,
              int http_status = 0,
              int esp_error = 0,
              int socket_error = 0,
              int tls_error = 0,
              int tls_flags = 0);

    bool cancelled() const;
    void set_error(const char *error);
    static bool write_callback(void *ctx,
                               size_t offset,
                               const uint8_t *data,
                               size_t len);
    static bool continue_callback(void *ctx);

    bool lock(TickType_t timeout = portMAX_DELAY) const;
    void unlock() const;

    FirmwareInstaller &installer_;

    FirmwareUrlSourceStatus status_;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    char *request_url_ = nullptr;
    size_t request_image_size_ = 0;
    size_t request_wire_size_ = 0;
    OtaUploadEncoding request_encoding_ = OtaUploadEncoding::Auto;
    TaskHandle_t task_ = nullptr;
    bool admission_pending_ = false;
    bool cancel_requested_ = false;
};

}  // namespace aircannect
