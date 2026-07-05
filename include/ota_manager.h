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

enum class OtaUploadEncoding : uint8_t {
    Plain,
    Zlib,
};

const char *ota_upload_encoding_name(OtaUploadEncoding encoding);
bool parse_ota_upload_encoding(const char *value, OtaUploadEncoding &out);

struct OtaManagerStatus {
    bool arduino_started = false;
    bool arduino_active = false;
    bool http_prepare_pending = false;
    bool http_prepared = false;
    bool http_active = false;
    bool http_ready = false;
    bool reboot_pending = false;
    bool auth_enabled = true;
    uint16_t arduino_port = AC_ARDUINO_OTA_PORT;
    size_t bytes = 0;
    size_t total_size = 0;
    size_t wire_bytes = 0;
    size_t wire_total_size = 0;
    uint8_t progress_percent = 0;
    String method = "idle";
    String encoding = "plain";
    String partition;
    String last_error;
};

class OtaManager {
public:
    void begin(AppConfig &app_config);
    void poll(const WifiManager &wifi_manager,
              bool reboot_allowed = true,
              bool arduino_ota_allowed = true,
              bool arduino_ota_poll_allowed = true);
    void mark_config_dirty();

    bool request_http_upload_prepare(
        size_t image_size,
        OtaUploadEncoding encoding = OtaUploadEncoding::Plain,
        size_t wire_size = 0);
    void poll_http_upload_prepare(bool as11_quiesced,
                                  bool as11_quiesce_timed_out);
    bool as11_quiesce_required() const;
    bool begin_http_upload(
        const String &filename,
        size_t image_size,
        OtaUploadEncoding encoding = OtaUploadEncoding::Plain,
        size_t wire_size = 0);
    bool write_http_upload(size_t index, const uint8_t *data, size_t len);
    bool finish_http_upload();
    void abort_http_upload(const char *reason);

    void schedule_reboot(uint32_t delay_ms = 750);

    bool active() const;
    OtaManagerStatus status() const;

private:
    void start_arduino_ota();
    void stop_arduino_ota();

    bool begin_zlib_decoder();
    void reset_zlib_decoder();
    bool write_plain_http_upload(size_t index, const uint8_t *data, size_t len);
    bool write_zlib_http_upload(size_t index, const uint8_t *data, size_t len);
    bool finish_zlib_http_upload();
    bool write_decompressed_http_bytes(const uint8_t *data, size_t len);
    bool apply_http_wire_progress(size_t bytes);
    bool apply_http_progress(size_t bytes);
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
    size_t prepared_image_size_ = 0;
    size_t prepared_wire_size_ = 0;
    uint32_t http_prepared_at_ms_ = 0;
    uint32_t http_upload_last_activity_ms_ = 0;
    OtaUploadEncoding prepared_encoding_ = OtaUploadEncoding::Plain;
    OtaUploadEncoding http_encoding_ = OtaUploadEncoding::Plain;
    void *zlib_decoder_ = nullptr;
    uint8_t *zlib_dict_ = nullptr;
    size_t zlib_output_offset_ = 0;
    bool zlib_finished_ = false;
    bool http_image_magic_checked_ = false;
    bool http_write_in_progress_ = false;
    OtaManagerStatus status_;
    mutable SemaphoreHandle_t status_mutex_ = nullptr;
};

}  // namespace aircannect
