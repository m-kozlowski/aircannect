#pragma once

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

namespace aircannect {

enum class OtaUploadEncoding : uint8_t {
    Auto,
    Plain,
    Zlib,
};

enum class FirmwareInstallSource : uint8_t {
    None,
    HttpUpload,
    Url,
    Arduino,
};

const char *ota_upload_encoding_name(OtaUploadEncoding encoding);
bool parse_ota_upload_encoding(const char *value, OtaUploadEncoding &out);
const char *firmware_install_source_name(FirmwareInstallSource source);

struct FirmwareInstallStatus {
    FirmwareInstallSource source = FirmwareInstallSource::None;
    OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
    bool source_reserved = false;
    bool prepare_pending = false;
    bool prepared = false;
    bool writing = false;
    bool ready = false;
    bool reboot_pending = false;
    size_t bytes = 0;
    size_t total_size = 0;
    size_t wire_bytes = 0;
    size_t wire_total_size = 0;
    uint8_t progress_percent = 0;
    String partition;
    String last_error;
};

class FirmwareInstaller {
public:
    void begin();
    void poll(bool reboot_allowed = true);

    // Source admission
    bool reserve_source(FirmwareInstallSource source,
                        OtaUploadEncoding encoding = OtaUploadEncoding::Auto,
                        size_t image_size = 0,
                        size_t wire_size = 0);
    bool owned_by(FirmwareInstallSource source) const;

    // Prepared partition writer
    bool request_prepare(size_t image_size,
                         OtaUploadEncoding encoding,
                         size_t wire_size,
                         FirmwareInstallSource source);
    void poll_prepare(bool as11_quiesced, bool as11_quiesce_timed_out);
    bool begin_write(const String &filename,
                     size_t image_size,
                     OtaUploadEncoding encoding,
                     size_t wire_size,
                     FirmwareInstallSource source);
    bool write(size_t index, const uint8_t *data, size_t len);
    bool finish();
    void abort(const char *reason, bool log_error = true);

    // External writers such as ArduinoOTA
    bool begin_external_install(FirmwareInstallSource source);
    void update_external_progress(size_t bytes, size_t total_size);
    void complete_external_install();
    void fail_external_install(const char *reason);

    // Lifecycle and query
    void schedule_reboot(uint32_t delay_ms = 750);
    bool active() const;
    bool as11_quiesce_required() const;
    FirmwareInstallStatus status() const;

private:
    // Partition writer
    bool begin_zlib_decoder();
    void reset_zlib_decoder();
    bool write_auto(size_t index, const uint8_t *data, size_t len);
    bool resolve_encoding(const uint8_t header[2]);
    bool write_plain(size_t index, const uint8_t *data, size_t len);
    bool write_zlib(size_t index, const uint8_t *data, size_t len);
    bool finish_zlib();
    bool write_decompressed_bytes(const uint8_t *data, size_t len);
    bool apply_progress(size_t bytes);
    bool apply_wire_progress(size_t bytes);

    // Shared state
    bool source_available_locked() const;
    void set_error_locked(const char *error);
    void clear_install_state_locked();
    bool lock(TickType_t timeout = portMAX_DELAY) const;
    void unlock() const;

    FirmwareInstallStatus status_;
    mutable SemaphoreHandle_t mutex_ = nullptr;

    uint32_t reboot_at_ms_ = 0;
    bool reboot_wait_logged_ = false;
    uint32_t last_progress_log_percent_ = 255;

    esp_ota_handle_t ota_handle_ = 0;
    const esp_partition_t *partition_ = nullptr;
    size_t prepared_image_size_ = 0;
    size_t prepared_wire_size_ = 0;
    uint32_t prepared_at_ms_ = 0;
    uint32_t write_last_activity_ms_ = 0;
    OtaUploadEncoding prepared_encoding_ = OtaUploadEncoding::Auto;
    OtaUploadEncoding write_encoding_ = OtaUploadEncoding::Auto;
    uint8_t probe_bytes_[2] = {};
    size_t probe_size_ = 0;
    void *zlib_decoder_ = nullptr;
    uint8_t *zlib_dict_ = nullptr;
    size_t zlib_output_offset_ = 0;
    bool zlib_finished_ = false;
    bool image_magic_checked_ = false;
};

}  // namespace aircannect
