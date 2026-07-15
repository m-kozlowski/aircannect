#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdint.h>

#include "app_config.h"
#include "ota_release_manifest.h"
#include "wifi_manager.h"

class ArduinoOTAClass;

namespace aircannect {

struct OtaUrlError;

enum class OtaUploadEncoding : uint8_t {
    Auto,
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

struct OtaUpdateNotification {
    bool checking = false;
    bool available = false;
    char version[AC_OTA_RELEASE_VERSION_MAX] = {};
};

class OtaManager {
public:
    void begin(AppConfig &app_config);
    void poll(const WifiManager &wifi_manager,
              bool reboot_allowed = true,
              bool arduino_ota_allowed = true,
              bool arduino_ota_poll_allowed = true,
              bool update_check_allowed = true);
    void mark_config_dirty();
    void mark_update_config_dirty();

    bool request_http_upload_prepare(
        size_t image_size,
        OtaUploadEncoding encoding = OtaUploadEncoding::Auto,
        size_t wire_size = 0);
    void poll_http_upload_prepare(bool as11_quiesced,
                                  bool as11_quiesce_timed_out);
    bool as11_quiesce_required() const;
    bool begin_http_upload(
        const String &filename,
        size_t image_size,
        OtaUploadEncoding encoding = OtaUploadEncoding::Auto,
        size_t wire_size = 0);
    bool write_http_upload(size_t index, const uint8_t *data, size_t len);
    bool finish_http_upload();
    void abort_http_upload(const char *reason);

    bool request_url_update(
        const String &url,
        OtaUploadEncoding encoding = OtaUploadEncoding::Auto,
        size_t image_size = 0,
        size_t wire_size = 0);
    bool request_update_check();
    bool request_available_update();
    void request_abort(const char *reason);

    void schedule_reboot(uint32_t delay_ms = 750);

    bool active() const;
    OtaManagerStatus status() const;
    OtaUpdateNotification update_notification() const;

private:
    struct UpdateArtifact {
        char *url = nullptr;
        size_t image_size = 0;
        size_t wire_size = 0;
        OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
    };

    enum class InstallSource : uint8_t {
        HttpUpload,
        Url,
    };

    void start_arduino_ota();
    void stop_arduino_ota();

    bool request_upload_prepare(size_t image_size,
                                OtaUploadEncoding encoding,
                                size_t wire_size,
                                InstallSource source);
    bool begin_upload(const String &filename,
                      size_t image_size,
                      OtaUploadEncoding encoding,
                      size_t wire_size,
                      InstallSource source);
    void abort_upload(const char *reason, bool log_error);

    static void url_task_entry(void *ctx);
    void run_url_task();
    void finish_url_task(char *url);
    void fail_url_update(const char *reason,
                         int http_status = 0,
                         int esp_error = 0,
                         int socket_error = 0,
                         int tls_error = 0,
                         int tls_flags = 0);
    bool url_cancelled() const;
    static bool url_write_callback(void *ctx,
                                   size_t offset,
                                   const uint8_t *data,
                                   size_t len);
    static bool url_continue_callback(void *ctx);

    void poll_update_check(bool network_online, bool check_allowed);
    bool start_update_check();
    static void update_check_task_entry(void *ctx);
    void run_update_check_task();
    void finish_update_check(char *url,
                             uint32_t generation,
                             const OtaReleaseManifest *manifest,
                             bool update_available,
                             UpdateArtifact artifact,
                             const char *error,
                             bool retry_soon,
                             const OtaUrlError *transport_error = nullptr);
    void clear_available_update_locked();
    bool update_check_cancelled(uint32_t generation) const;
    static bool update_check_continue_callback(void *ctx);

    bool begin_zlib_decoder();
    void reset_zlib_decoder();
    bool write_auto_http_upload(size_t index, const uint8_t *data, size_t len);
    bool resolve_http_upload_encoding(const uint8_t header[2]);
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
    InstallSource install_source_ = InstallSource::HttpUpload;
    uint32_t http_prepared_at_ms_ = 0;
    uint32_t http_upload_last_activity_ms_ = 0;
    OtaUploadEncoding prepared_encoding_ = OtaUploadEncoding::Auto;
    OtaUploadEncoding http_encoding_ = OtaUploadEncoding::Auto;
    uint8_t http_probe_bytes_[2] = {};
    size_t http_probe_size_ = 0;
    void *zlib_decoder_ = nullptr;
    uint8_t *zlib_dict_ = nullptr;
    size_t zlib_output_offset_ = 0;
    bool zlib_finished_ = false;
    bool http_image_magic_checked_ = false;
    bool http_write_in_progress_ = false;

    char *url_request_ = nullptr;
    size_t url_request_image_size_ = 0;
    size_t url_request_wire_size_ = 0;
    OtaUploadEncoding url_request_encoding_ = OtaUploadEncoding::Auto;
    TaskHandle_t url_task_ = nullptr;
    bool url_cancel_requested_ = false;

    bool update_config_dirty_ = false;
    bool update_network_online_ = false;
    bool update_manual_requested_ = false;
    bool update_check_cancel_requested_ = false;
    uint32_t update_network_since_ms_ = 0;
    uint32_t update_next_check_ms_ = 0;
    uint32_t update_last_check_ms_ = 0;
    uint32_t update_config_generation_ = 1;
    uint32_t update_task_generation_ = 0;
    char *update_check_url_ = nullptr;
    TaskHandle_t update_check_task_ = nullptr;
    UpdateArtifact available_update_;

    OtaManagerStatus status_;
    mutable SemaphoreHandle_t status_mutex_ = nullptr;
};

}  // namespace aircannect
