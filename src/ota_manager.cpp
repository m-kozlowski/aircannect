#include "ota_manager.h"

#include <algorithm>
#include <cstring>

#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <miniz.h>

#include "debug_log.h"
#include "memory_manager.h"

namespace aircannect {

namespace {

static constexpr size_t kHttpOtaWriteChunkBytes = 4096;
static constexpr uint32_t kHttpOtaPreparedTtlMs = 60000;
static constexpr uint32_t kHttpOtaUploadIdleTimeoutMs = 60000;

const char *arduino_ota_error_name(ota_error_t error) {
    switch (error) {
        case OTA_AUTH_ERROR: return "auth_failed";
        case OTA_BEGIN_ERROR: return "begin_failed";
        case OTA_CONNECT_ERROR: return "connect_failed";
        case OTA_RECEIVE_ERROR: return "receive_failed";
        case OTA_END_ERROR: return "end_failed";
        default: return "unknown";
    }
}

}  // namespace

const char *ota_upload_encoding_name(OtaUploadEncoding encoding) {
    switch (encoding) {
        case OtaUploadEncoding::Zlib: return "zlib";
        case OtaUploadEncoding::Plain:
        default: return "plain";
    }
}

bool parse_ota_upload_encoding(const char *value, OtaUploadEncoding &out) {
    if (!value || !*value || strcmp(value, "plain") == 0) {
        out = OtaUploadEncoding::Plain;
        return true;
    }
    if (strcmp(value, "zlib") == 0) {
        out = OtaUploadEncoding::Zlib;
        return true;
    }
    return false;
}

void OtaManager::begin(AppConfig &app_config) {
    app_config_ = &app_config;
    if (!status_mutex_) {
        status_mutex_ = xSemaphoreCreateRecursiveMutex();
    }
    if (!lock_status()) return;
    status_.auth_enabled = app_config.data().ota_password.length() > 0;
    status_.arduino_port = AC_ARDUINO_OTA_PORT;
    unlock_status();
    esp_ota_mark_app_valid_cancel_rollback();
}

void OtaManager::poll(const WifiManager &wifi_manager,
                      bool reboot_allowed,
                      bool arduino_ota_allowed,
                      bool arduino_ota_poll_allowed) {
    if (reboot_at_ms_ &&
        static_cast<int32_t>(millis() - reboot_at_ms_) >= 0) {
        if (!reboot_allowed) {
            if (!reboot_wait_logged_) {
                reboot_wait_logged_ = true;
                Log::logf(CAT_OTA, LOG_INFO,
                          "reboot waiting for AS11 quiesce\n");
            }
            return;
        }
        Log::logf(CAT_OTA, LOG_INFO, "rebooting\n");
        delay(50);
        ESP.restart();
    }

    if (!app_config_) return;

    if (!lock_status()) return;
    const uint32_t now = millis();
    status_.auth_enabled = app_config_->data().ota_password.length() > 0;
    if (status_.http_prepared && http_prepared_at_ms_ &&
        static_cast<int32_t>(now - http_prepared_at_ms_) >=
            static_cast<int32_t>(kHttpOtaPreparedTtlMs)) {
        status_.http_prepared = false;
        prepared_image_size_ = 0;
        prepared_wire_size_ = 0;
        prepared_encoding_ = OtaUploadEncoding::Plain;
        http_prepared_at_ms_ = 0;
        http_partition_ = nullptr;
        status_.method = "idle";
        status_.partition = "";
        status_.total_size = 0;
        status_.wire_total_size = 0;
        status_.last_error = "ota_prepare_expired";
        Log::logf(CAT_OTA, LOG_WARN,
                  "HTTP upload prepare expired before upload\n");
    }
    if (status_.http_active && http_upload_last_activity_ms_ &&
        !http_write_in_progress_ &&
        static_cast<int32_t>(now - http_upload_last_activity_ms_) >=
            static_cast<int32_t>(kHttpOtaUploadIdleTimeoutMs)) {
        unlock_status();
        abort_http_upload("http_upload_timeout");
        return;
    }
    if (!wifi_manager.network_available()) {
        const bool stop_ota = status_.arduino_started;
        unlock_status();
        if (stop_ota) stop_arduino_ota();
        return;
    }
    if (!arduino_ota_allowed) {
        const bool stop_ota = status_.arduino_started;
        unlock_status();
        if (stop_ota) stop_arduino_ota();
        return;
    }
    if (status_.http_prepare_pending || status_.http_prepared ||
        status_.http_active || status_.http_ready || status_.reboot_pending) {
        const bool stop_ota = status_.arduino_started;
        unlock_status();
        if (stop_ota) stop_arduino_ota();
        return;
    }
    if (!arduino_ota_poll_allowed) {
        unlock_status();
        return;
    }

    const bool start_ota = !status_.arduino_started || arduino_config_dirty_;
    unlock_status();
    if (start_ota) {
        start_arduino_ota();
    }
    if (arduino_ota_) arduino_ota_->handle();
}

void OtaManager::mark_config_dirty() {
    arduino_config_dirty_ = true;
}

bool OtaManager::active() const {
    if (!lock_status()) return false;
    const bool result = status_.http_prepare_pending ||
                        status_.http_prepared || status_.http_active ||
                        status_.http_ready || arduino_active_ ||
                        status_.reboot_pending;
    unlock_status();
    return result;
}

OtaManagerStatus OtaManager::status() const {
    if (!lock_status()) return OtaManagerStatus();
    OtaManagerStatus copy = status_;
    unlock_status();
    return copy;
}

bool OtaManager::request_http_upload_prepare(size_t image_size,
                                             OtaUploadEncoding encoding,
                                             size_t wire_size) {
    if (wire_size == 0) wire_size = image_size;

    if (!lock_status()) return false;
    if (status_.http_active || status_.http_ready || status_.reboot_pending) {
        set_error("ota_busy");
        unlock_status();
        return false;
    }
    if (status_.http_prepared && prepared_image_size_ == image_size &&
        prepared_encoding_ == encoding && prepared_wire_size_ == wire_size) {
        unlock_status();
        return true;
    }
    if (encoding == OtaUploadEncoding::Zlib && wire_size == 0) {
        set_error("missing_wire_size");
        unlock_status();
        return false;
    }

    clear_http_state();
    status_.method = "http_prepare";
    status_.encoding = ota_upload_encoding_name(encoding);
    status_.http_prepare_pending = true;
    status_.bytes = 0;
    status_.total_size = image_size;
    status_.wire_bytes = 0;
    status_.wire_total_size = wire_size;
    status_.progress_percent = 0;
    status_.partition = "";
    status_.last_error = "";
    prepared_image_size_ = image_size;
    prepared_wire_size_ = wire_size;
    prepared_encoding_ = encoding;
    http_prepared_at_ms_ = 0;

    http_partition_ = esp_ota_get_next_update_partition(nullptr);
    if (!http_partition_) {
        status_.http_prepare_pending = false;
        prepared_image_size_ = 0;
        prepared_wire_size_ = 0;
        prepared_encoding_ = OtaUploadEncoding::Plain;
        status_.method = "idle";
        set_error("no_ota_partition");
        unlock_status();
        return false;
    }
    if (image_size == 0 || image_size > http_partition_->size) {
        status_.http_prepare_pending = false;
        http_partition_ = nullptr;
        prepared_image_size_ = 0;
        prepared_wire_size_ = 0;
        prepared_encoding_ = OtaUploadEncoding::Plain;
        status_.method = "idle";
        set_error("image_size_invalid");
        unlock_status();
        return false;
    }

    status_.partition = http_partition_->label;
    Log::logf(CAT_OTA, LOG_INFO,
              "HTTP upload prepare partition=%s image_size=%u encoding=%s "
              "wire_size=%u\n",
              http_partition_->label, static_cast<unsigned>(image_size),
              ota_upload_encoding_name(encoding),
              static_cast<unsigned>(wire_size));
    unlock_status();
    return true;
}

void OtaManager::poll_http_upload_prepare(bool as11_quiesced,
                                          bool as11_quiesce_timed_out) {
    if (!lock_status()) return;
    if (!status_.http_prepare_pending || status_.http_prepared) {
        unlock_status();
        return;
    }

    if (as11_quiesced) {
        status_.http_prepare_pending = false;
        status_.http_prepared = true;
        status_.method = "http";
        http_prepared_at_ms_ = millis();
        Log::logf(CAT_OTA, LOG_INFO,
                  "HTTP upload prepared; AS11 traffic quiesced\n");
        unlock_status();
        return;
    }

    if (as11_quiesce_timed_out) {
        status_.http_prepare_pending = false;
        status_.http_prepared = false;
        status_.method = "idle";
        status_.partition = "";
        status_.total_size = 0;
        status_.wire_total_size = 0;
        http_partition_ = nullptr;
        prepared_image_size_ = 0;
        prepared_wire_size_ = 0;
        prepared_encoding_ = OtaUploadEncoding::Plain;
        http_prepared_at_ms_ = 0;
        set_error("as11_quiesce_timeout");
        Log::logf(CAT_OTA, LOG_ERROR,
                  "HTTP upload prepare failed: AS11 quiesce timeout\n");
    }
    unlock_status();
}

bool OtaManager::as11_quiesce_required() const {
    if (!lock_status()) return false;
    const bool result = status_.http_prepare_pending ||
                        status_.http_prepared || status_.http_active ||
                        status_.http_ready || status_.reboot_pending ||
                        arduino_active_;
    unlock_status();
    return result;
}

bool OtaManager::begin_http_upload(const String &filename,
                                   size_t image_size,
                                   OtaUploadEncoding encoding,
                                   size_t wire_size) {
    if (wire_size == 0) wire_size = image_size;

    if (!lock_status()) return false;
    if (status_.http_active) {
        set_error("upload_already_active");
        unlock_status();
        return false;
    }
    if (!status_.http_prepared || prepared_image_size_ != image_size ||
        prepared_encoding_ != encoding || prepared_wire_size_ != wire_size) {
        set_error("ota_prepare_required");
        unlock_status();
        return false;
    }

    clear_http_state();
    status_.method = "http";
    status_.encoding = ota_upload_encoding_name(encoding);
    status_.http_active = true;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = millis();
    http_encoding_ = encoding;
    http_write_in_progress_ = false;
    http_image_magic_checked_ = false;
    status_.bytes = 0;
    status_.total_size = image_size;
    status_.wire_bytes = 0;
    status_.wire_total_size = wire_size;
    status_.progress_percent = 0;
    status_.partition = "";
    status_.last_error = "";

    http_partition_ = esp_ota_get_next_update_partition(nullptr);
    if (!http_partition_) {
        abort_http_upload("no_ota_partition");
        unlock_status();
        return false;
    }
    if (image_size == 0 || image_size > http_partition_->size) {
        abort_http_upload("image_size_invalid");
        unlock_status();
        return false;
    }

    status_.partition = http_partition_->label;
    esp_err_t err = esp_ota_begin(http_partition_, OTA_WITH_SEQUENTIAL_WRITES,
                                  &http_handle_);
    if (err != ESP_OK) {
        abort_http_upload(esp_err_to_name(err));
        unlock_status();
        return false;
    }
    if (encoding == OtaUploadEncoding::Zlib && !begin_zlib_decoder()) {
        abort_http_upload("zlib_alloc_failed");
        unlock_status();
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "HTTP upload start file=%s partition=%s image_size=%u "
              "encoding=%s wire_size=%u\n",
              filename.c_str(), http_partition_->label,
              static_cast<unsigned>(image_size),
              ota_upload_encoding_name(encoding),
              static_cast<unsigned>(wire_size));
    unlock_status();
    return true;
}

bool OtaManager::begin_zlib_decoder() {
    reset_zlib_decoder();

    zlib_decoder_ = Memory::calloc_large(1, sizeof(tinfl_decompressor), false);
    zlib_dict_ =
        static_cast<uint8_t *>(Memory::alloc_large(TINFL_LZ_DICT_SIZE, false));
    if (!zlib_decoder_ || !zlib_dict_) {
        reset_zlib_decoder();
        return false;
    }

    tinfl_init(static_cast<tinfl_decompressor *>(zlib_decoder_));
    zlib_output_offset_ = 0;
    zlib_finished_ = false;
    return true;
}

void OtaManager::reset_zlib_decoder() {
    if (zlib_decoder_) {
        Memory::free(zlib_decoder_);
    }
    if (zlib_dict_) {
        Memory::free(zlib_dict_);
    }

    zlib_decoder_ = nullptr;
    zlib_dict_ = nullptr;
    zlib_output_offset_ = 0;
    zlib_finished_ = false;
}

bool OtaManager::write_http_upload(size_t index, const uint8_t *data,
                                   size_t len) {
    if (!lock_status()) return false;
    const OtaUploadEncoding encoding = http_encoding_;
    unlock_status();

    if (encoding == OtaUploadEncoding::Zlib) {
        return write_zlib_http_upload(index, data, len);
    }
    return write_plain_http_upload(index, data, len);
}

bool OtaManager::write_plain_http_upload(size_t index, const uint8_t *data,
                                         size_t len) {
    if (!lock_status()) return false;
    if (!status_.http_active || !http_partition_ || !http_handle_) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        unlock_status();
        return false;
    }
    if (!data || len == 0) {
        http_upload_last_activity_ms_ = millis();
        unlock_status();
        return true;
    }

    if (index != status_.bytes) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "HTTP upload offset mismatch index=%u expected=%u len=%u "
                  "total=%u\n",
                  static_cast<unsigned>(index),
                  static_cast<unsigned>(status_.bytes),
                  static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.total_size));
        abort_http_upload("upload_offset_mismatch");
        unlock_status();
        return false;
    }
    if (status_.total_size == 0 || index > status_.total_size ||
        len > status_.total_size - index) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "HTTP upload overrun index=%u len=%u total=%u\n",
                  static_cast<unsigned>(index), static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.total_size));
        abort_http_upload("upload_overrun");
        unlock_status();
        return false;
    }
    if (index == 0 && data[0] != 0xE9) {
        abort_http_upload("bad_esp32_image");
        unlock_status();
        return false;
    }

    esp_ota_handle_t handle = http_handle_;
    http_upload_last_activity_ms_ = millis();
    http_write_in_progress_ = true;
    unlock_status();

    const char *write_error = nullptr;
    size_t offset = 0;
    while (offset < len) {
        const size_t chunk =
            std::min(kHttpOtaWriteChunkBytes, len - offset);
        esp_err_t err = esp_ota_write(handle, data + offset, chunk);
        if (err != ESP_OK) {
            write_error = esp_err_to_name(err);
            break;
        }
        if (!apply_http_progress(chunk)) {
            break;
        }
        offset += chunk;
        yield();
    }
    if (lock_status()) {
        http_write_in_progress_ = false;
        http_upload_last_activity_ms_ = millis();
        unlock_status();
    }
    if (write_error) {
        abort_http_upload(write_error);
        return false;
    }
    if (offset < len) return false;
    return true;
}

bool OtaManager::write_zlib_http_upload(size_t index, const uint8_t *data,
                                        size_t len) {
    if (!lock_status()) return false;
    if (!status_.http_active || !http_partition_ || !http_handle_ ||
        !zlib_decoder_ || !zlib_dict_) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        unlock_status();
        return false;
    }
    if (!data || len == 0) {
        http_upload_last_activity_ms_ = millis();
        unlock_status();
        return true;
    }

    if (index != status_.wire_bytes) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "HTTP zlib upload offset mismatch index=%u expected=%u "
                  "len=%u total=%u\n",
                  static_cast<unsigned>(index),
                  static_cast<unsigned>(status_.wire_bytes),
                  static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.wire_total_size));
        abort_http_upload("upload_offset_mismatch");
        unlock_status();
        return false;
    }
    if (status_.wire_total_size == 0 || index > status_.wire_total_size ||
        len > status_.wire_total_size - index) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "HTTP zlib upload overrun index=%u len=%u total=%u\n",
                  static_cast<unsigned>(index), static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.wire_total_size));
        abort_http_upload("upload_overrun");
        unlock_status();
        return false;
    }
    if (zlib_finished_) {
        abort_http_upload("zlib_trailing_data");
        unlock_status();
        return false;
    }

    tinfl_decompressor *decoder =
        static_cast<tinfl_decompressor *>(zlib_decoder_);
    uint8_t *dict = zlib_dict_;
    const size_t wire_total_size = status_.wire_total_size;
    http_upload_last_activity_ms_ = millis();
    http_write_in_progress_ = true;
    unlock_status();

    const uint8_t *input = data;
    size_t remaining = len;
    bool drain_decoder = true;
    bool done = false;
    const char *decode_error = nullptr;

    while (remaining > 0 || drain_decoder) {
        drain_decoder = false;

        const size_t before_remaining = remaining;
        size_t in_size = remaining;
        const size_t out_pos =
            zlib_output_offset_ & (TINFL_LZ_DICT_SIZE - 1);
        size_t out_size = TINFL_LZ_DICT_SIZE - out_pos;

        uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
                         TINFL_FLAG_COMPUTE_ADLER32;
        if (index + (len - remaining) + in_size < wire_total_size) {
            flags |= TINFL_FLAG_HAS_MORE_INPUT;
        }

        const tinfl_status result = tinfl_decompress(
            decoder, input, &in_size, dict, dict + out_pos, &out_size, flags);

        if (out_size > 0 &&
            !write_decompressed_http_bytes(dict + out_pos, out_size)) {
            decode_error = "ota_write_failed";
            break;
        }

        input += in_size;
        remaining -= in_size;
        zlib_output_offset_ += out_size;

        if (result == TINFL_STATUS_DONE) {
            done = true;
            if (remaining != 0) decode_error = "zlib_trailing_data";
            break;
        }
        if (result == TINFL_STATUS_NEEDS_MORE_INPUT) {
            if (remaining == 0) break;
        } else if (result == TINFL_STATUS_HAS_MORE_OUTPUT) {
            drain_decoder = true;
        } else {
            decode_error = result == TINFL_STATUS_ADLER32_MISMATCH
                               ? "zlib_checksum_failed"
                               : "zlib_decode_failed";
            Log::logf(CAT_OTA, LOG_ERROR,
                      "HTTP zlib stream failed status=%d in=%u/%u "
                      "out=%u total_out=%u\n",
                      static_cast<int>(result),
                      static_cast<unsigned>(len - remaining),
                      static_cast<unsigned>(len),
                      static_cast<unsigned>(out_size),
                      static_cast<unsigned>(zlib_output_offset_));
            break;
        }

        if (in_size == 0 && out_size == 0 &&
            before_remaining == remaining) {
            decode_error = "zlib_decode_stalled";
            break;
        }

        yield();
    }

    if (lock_status()) {
        if (done) zlib_finished_ = true;
        http_write_in_progress_ = false;
        http_upload_last_activity_ms_ = millis();
        unlock_status();
    }

    if (decode_error) {
        abort_http_upload(decode_error);
        return false;
    }
    if (remaining != 0) {
        abort_http_upload("zlib_decode_incomplete");
        return false;
    }
    if (!apply_http_wire_progress(len)) {
        return false;
    }
    return true;
}

bool OtaManager::finish_zlib_http_upload() {
    if (!lock_status()) return false;
    if (status_.wire_bytes != status_.wire_total_size) {
        abort_http_upload("incomplete_wire_upload");
        unlock_status();
        return false;
    }
    if (!zlib_finished_) {
        abort_http_upload("zlib_stream_incomplete");
        unlock_status();
        return false;
    }
    unlock_status();
    return true;
}

bool OtaManager::write_decompressed_http_bytes(const uint8_t *data,
                                               size_t len) {
    if (!data || len == 0) return true;

    if (!http_image_magic_checked_) {
        if (data[0] != 0xE9) {
            abort_http_upload("bad_esp32_image");
            return false;
        }
        http_image_magic_checked_ = true;
    }

    size_t offset = 0;
    while (offset < len) {
        const size_t chunk =
            std::min(kHttpOtaWriteChunkBytes, len - offset);
        esp_err_t err = esp_ota_write(http_handle_, data + offset, chunk);
        if (err != ESP_OK) {
            abort_http_upload(esp_err_to_name(err));
            return false;
        }
        if (!apply_http_progress(chunk)) {
            return false;
        }
        offset += chunk;
        yield();
    }

    return true;
}

bool OtaManager::finish_http_upload() {
    if (!lock_status()) return false;
    if (!status_.http_active || !http_partition_ || !http_handle_) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        unlock_status();
        return false;
    }
    const bool zlib = http_encoding_ == OtaUploadEncoding::Zlib;
    if (zlib) {
        unlock_status();
        if (!finish_zlib_http_upload()) return false;
        if (!lock_status()) return false;
    }
    if (status_.total_size == 0 || status_.bytes != status_.total_size) {
        abort_http_upload("incomplete_upload");
        unlock_status();
        return false;
    }

    esp_err_t err = esp_ota_end(http_handle_);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(http_partition_);
    }

    if (err != ESP_OK) {
        abort_http_upload(esp_err_to_name(err));
        unlock_status();
        return false;
    }

    status_.http_active = false;
    status_.http_ready = true;
    status_.http_prepare_pending = false;
    status_.http_prepared = false;
    prepared_image_size_ = 0;
    prepared_wire_size_ = 0;
    prepared_encoding_ = OtaUploadEncoding::Plain;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = 0;
    http_write_in_progress_ = false;
    reset_zlib_decoder();
    status_.progress_percent = 100;
    status_.last_error = "";
    Log::logf(CAT_OTA, LOG_INFO,
              "HTTP upload complete bytes=%u wire_bytes=%u encoding=%s "
              "partition=%s; rebooting\n",
              static_cast<unsigned>(status_.bytes),
              static_cast<unsigned>(status_.wire_bytes),
              ota_upload_encoding_name(http_encoding_),
              http_partition_->label);
    http_encoding_ = OtaUploadEncoding::Plain;
    http_handle_ = 0;
    http_partition_ = nullptr;
    schedule_reboot(2000);
    unlock_status();
    return true;
}

void OtaManager::abort_http_upload(const char *reason) {
    if (!lock_status()) return;
    if (http_handle_) esp_ota_abort(http_handle_);
    reset_zlib_decoder();
    http_handle_ = 0;
    http_partition_ = nullptr;
    prepared_image_size_ = 0;
    prepared_wire_size_ = 0;
    prepared_encoding_ = OtaUploadEncoding::Plain;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = 0;
    http_encoding_ = OtaUploadEncoding::Plain;
    http_image_magic_checked_ = false;
    http_write_in_progress_ = false;
    status_.http_prepare_pending = false;
    status_.http_prepared = false;
    status_.http_active = false;
    status_.http_ready = false;
    status_.method = "idle";
    status_.partition = "";
    set_error(reason ? reason : "aborted");
    Log::logf(CAT_OTA, LOG_ERROR, "HTTP upload failed: %s\n",
              status_.last_error.c_str());
    unlock_status();
}

void OtaManager::schedule_reboot(uint32_t delay_ms) {
    if (!lock_status()) return;
    reboot_at_ms_ = millis() + delay_ms;
    reboot_wait_logged_ = false;
    status_.reboot_pending = true;
    unlock_status();
}

void OtaManager::start_arduino_ota() {
    if (!app_config_) return;

    stop_arduino_ota();

    // Allocate and configure endpoint
    arduino_ota_ = new ArduinoOTAClass();
    if (!arduino_ota_) {
        set_error("arduino_ota_alloc_failed");
        return;
    }

    const AppConfigData &cfg = app_config_->data();

    arduino_ota_->setHostname(cfg.hostname.c_str());
    arduino_ota_->setPort(AC_ARDUINO_OTA_PORT);
    arduino_ota_->setMdnsEnabled(false);
    arduino_ota_->setRebootOnSuccess(false);

    if (cfg.ota_password.length()) {
        arduino_ota_->setPassword(cfg.ota_password.c_str());
    }

    // Upload lifecycle callbacks
    arduino_ota_->onStart([this]() {
        const char *type =
            arduino_ota_->getCommand() == U_FLASH ? "firmware" : "filesystem";

        if (lock_status()) {
            arduino_active_ = true;
            status_.arduino_active = true;
            status_.method = "arduino";
            status_.bytes = 0;
            status_.total_size = 0;
            status_.progress_percent = 0;
            status_.last_error = "";
            last_progress_log_percent_ = 255;
            unlock_status();
        }

        Log::logf(CAT_OTA, LOG_INFO, "ArduinoOTA start %s\n", type);
    });

    arduino_ota_->onEnd([this]() {
        if (lock_status()) {
            arduino_active_ = false;
            status_.arduino_active = false;
            status_.progress_percent = 100;
            unlock_status();
        }

        schedule_reboot(2000);

        Log::logf(CAT_OTA, LOG_INFO,
                  "ArduinoOTA complete; rebooting\n");
    });

    arduino_ota_->onProgress([this](unsigned int progress,
                                    unsigned int total) {
        if (!lock_status()) return;

        status_.bytes = progress;
        status_.total_size = total;

        if (total > 0) {
            status_.progress_percent =
                static_cast<uint8_t>((progress * 100ULL) / total);
        }

        if (status_.progress_percent != last_progress_log_percent_ &&
            (status_.progress_percent % 10 == 0 ||
             status_.progress_percent == 100)) {
            last_progress_log_percent_ = status_.progress_percent;
            Log::logf(CAT_OTA, LOG_DEBUG, "ArduinoOTA %u%%\n",
                      static_cast<unsigned>(status_.progress_percent));
        }

        unlock_status();
    });

    arduino_ota_->onError([this](ota_error_t error) {
        Update.abort();

        const char *name = arduino_ota_error_name(error);

        if (lock_status()) {
            arduino_active_ = false;
            status_.arduino_active = false;
            status_.last_error = name;
            unlock_status();
        }

        arduino_config_dirty_ = true;

        Log::logf(CAT_OTA, LOG_ERROR, "ArduinoOTA error: %s\n", name);
    });

    // Publish service state
    arduino_ota_->begin();

    if (!lock_status()) return;

    status_.arduino_started = true;
    unlock_status();

    arduino_config_dirty_ = false;

    Log::logf(CAT_OTA, LOG_INFO,
              "ArduinoOTA ready port=%u auth=%s mdns=off\n",
              static_cast<unsigned>(AC_ARDUINO_OTA_PORT),
              cfg.ota_password.length() ? "on" : "off");
}

void OtaManager::stop_arduino_ota() {
    if (arduino_ota_) {
        arduino_ota_->end();
        delete arduino_ota_;
        arduino_ota_ = nullptr;
    }
    if (!lock_status()) return;
    arduino_active_ = false;
    status_.arduino_active = false;
    status_.arduino_started = false;
    unlock_status();
}

bool OtaManager::apply_http_progress(size_t bytes) {
    if (!lock_status()) return false;
    if (!status_.http_active || status_.total_size == 0 ||
        status_.bytes + bytes > status_.total_size) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        unlock_status();
        return false;
    }
    status_.bytes += bytes;
    http_upload_last_activity_ms_ = millis();
    status_.progress_percent =
        static_cast<uint8_t>((status_.bytes * 100ULL) / status_.total_size);
    if (status_.progress_percent != last_progress_log_percent_ &&
        (status_.progress_percent % 10 == 0 ||
         status_.progress_percent == 100)) {
        last_progress_log_percent_ = status_.progress_percent;
        Log::logf(CAT_OTA, LOG_DEBUG, "HTTP upload %u%%\n",
                  static_cast<unsigned>(status_.progress_percent));
    }
    unlock_status();
    return true;
}

bool OtaManager::apply_http_wire_progress(size_t bytes) {
    if (!lock_status()) return false;
    if (!status_.http_active || status_.wire_total_size == 0 ||
        status_.wire_bytes + bytes > status_.wire_total_size) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        unlock_status();
        return false;
    }

    status_.wire_bytes += bytes;
    http_upload_last_activity_ms_ = millis();
    unlock_status();
    return true;
}

void OtaManager::set_error(const char *error) {
    if (!lock_status()) return;
    status_.last_error = error ? error : "error";
    unlock_status();
}

void OtaManager::clear_http_state() {
    if (!lock_status()) return;
    reset_zlib_decoder();
    http_handle_ = 0;
    http_partition_ = nullptr;
    prepared_image_size_ = 0;
    prepared_wire_size_ = 0;
    prepared_encoding_ = OtaUploadEncoding::Plain;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = 0;
    http_encoding_ = OtaUploadEncoding::Plain;
    http_image_magic_checked_ = false;
    http_write_in_progress_ = false;
    status_.http_prepare_pending = false;
    status_.http_prepared = false;
    status_.http_active = false;
    status_.http_ready = false;
    status_.method = "idle";
    status_.encoding = "plain";
    status_.partition = "";
    status_.bytes = 0;
    status_.total_size = 0;
    status_.wire_bytes = 0;
    status_.wire_total_size = 0;
    status_.progress_percent = 0;
    last_progress_log_percent_ = 255;
    unlock_status();
}

bool OtaManager::lock_status(TickType_t timeout) const {
    return !status_mutex_ ||
           xSemaphoreTakeRecursive(status_mutex_, timeout) == pdTRUE;
}

void OtaManager::unlock_status() const {
    if (status_mutex_) xSemaphoreGiveRecursive(status_mutex_);
}

}  // namespace aircannect
