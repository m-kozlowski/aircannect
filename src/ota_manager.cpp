#include "ota_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <freertos/task.h>
#include <miniz.h>

#include "board_net.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "ota_url_client.h"

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
        case OtaUploadEncoding::Auto: return "auto";
        case OtaUploadEncoding::Zlib: return "zlib";
        case OtaUploadEncoding::Plain:
        default: return "plain";
    }
}

bool parse_ota_upload_encoding(const char *value, OtaUploadEncoding &out) {
    if (!value || !*value || strcmp(value, "auto") == 0) {
        out = OtaUploadEncoding::Auto;
        return true;
    }
    if (strcmp(value, "plain") == 0) {
        out = OtaUploadEncoding::Plain;
        return true;
    }
    if (strcmp(value, "zlib") == 0) {
        out = OtaUploadEncoding::Zlib;
        return true;
    }
    return false;
}

void OtaManager::begin(const AppConfigData &app_config) {
    app_config_ = &app_config;
    if (!status_mutex_) {
        status_mutex_ = xSemaphoreCreateRecursiveMutex();
    }
    if (!lock_status()) return;
    status_.auth_enabled = app_config.ota_password.length() > 0;
    status_.arduino_port = AC_ARDUINO_OTA_PORT;
    status_.update_check_enabled =
        app_config.update_url.length() > 0 &&
        AC_OTA_RELEASE_TARGET[0];
    unlock_status();
    esp_ota_mark_app_valid_cancel_rollback();
}

void OtaManager::poll(const WifiManager &wifi_manager,
                      bool reboot_allowed,
                      bool arduino_ota_allowed,
                      bool arduino_ota_poll_allowed,
                      bool update_check_allowed) {
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

    poll_update_check(wifi_manager.sta_ipv4_online(),
                      update_check_allowed && arduino_ota_allowed);

    if (!lock_status()) return;
    const uint32_t now = millis();
    status_.auth_enabled = app_config_->ota_password.length() > 0;
    if (status_.http_prepared && http_prepared_at_ms_ &&
        static_cast<int32_t>(now - http_prepared_at_ms_) >=
            static_cast<int32_t>(kHttpOtaPreparedTtlMs)) {
        status_.http_prepared = false;
        prepared_image_size_ = 0;
        prepared_wire_size_ = 0;
        prepared_encoding_ = OtaUploadEncoding::Auto;
        http_prepared_at_ms_ = 0;
        http_partition_ = nullptr;
        status_.method = "idle";
        status_.partition = "";
        status_.total_size = 0;
        status_.wire_total_size = 0;
        status_.last_error = "ota_prepare_expired";
        Log::logf(CAT_OTA, LOG_WARN,
                  "ESP OTA prepare expired source=%s\n",
                  install_source_ == InstallSource::Url ? "url" : "http_upload");
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
        status_.http_active || status_.http_ready || status_.url_active ||
        status_.update_check_active || status_.reboot_pending) {
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
                        status_.http_ready || status_.url_active ||
                        url_task_ != nullptr || status_.update_check_active ||
                        update_check_task_ != nullptr || arduino_active_ ||
                        status_.reboot_pending;
    unlock_status();
    return result;
}

OtaManagerStatus OtaManager::status() const {
    if (!lock_status()) return OtaManagerStatus();
    OtaManagerStatus copy = status_;
    if (copy.update_check_attempted && update_last_check_ms_) {
        copy.update_last_check_age_ms = millis() - update_last_check_ms_;
    }
    unlock_status();
    return copy;
}

OtaUpdateNotification OtaManager::update_notification() const {
    OtaUpdateNotification notification;
    if (!lock_status()) return notification;

    notification.checking = status_.update_check_pending ||
                            status_.update_check_active;
    notification.available = status_.update_available;
    snprintf(notification.version, sizeof(notification.version), "%s",
             status_.update_version.c_str());

    unlock_status();
    return notification;
}

bool OtaManager::request_http_upload_prepare(size_t image_size,
                                             OtaUploadEncoding encoding,
                                             size_t wire_size) {
    return request_upload_prepare(image_size, encoding, wire_size,
                                  InstallSource::HttpUpload);
}

bool OtaManager::request_upload_prepare(size_t image_size,
                                        OtaUploadEncoding encoding,
                                        size_t wire_size,
                                        InstallSource source) {
    if (wire_size == 0) wire_size = image_size;

    if (!lock_status()) return false;
    if ((source == InstallSource::HttpUpload && status_.url_active) ||
        status_.update_check_active || update_check_task_ ||
        status_.http_active || status_.http_ready || status_.reboot_pending) {
        set_error("ota_busy");
        unlock_status();
        return false;
    }
    if (status_.http_prepared && prepared_image_size_ == image_size &&
        prepared_encoding_ == encoding && prepared_wire_size_ == wire_size &&
        install_source_ == source) {
        unlock_status();
        return true;
    }
    if (encoding == OtaUploadEncoding::Zlib && wire_size == 0) {
        set_error("missing_wire_size");
        unlock_status();
        return false;
    }

    clear_http_state();
    install_source_ = source;
    status_.method = source == InstallSource::Url ? "url" : "http_prepare";
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
        prepared_encoding_ = OtaUploadEncoding::Auto;
        status_.method = "idle";
        set_error("no_ota_partition");
        unlock_status();
        return false;
    }
    if ((encoding == OtaUploadEncoding::Plain && image_size == 0) ||
        image_size > http_partition_->size) {
        status_.http_prepare_pending = false;
        http_partition_ = nullptr;
        prepared_image_size_ = 0;
        prepared_wire_size_ = 0;
        prepared_encoding_ = OtaUploadEncoding::Auto;
        status_.method = "idle";
        set_error("image_size_invalid");
        unlock_status();
        return false;
    }
    if (wire_size == 0) {
        status_.http_prepare_pending = false;
        http_partition_ = nullptr;
        prepared_image_size_ = 0;
        prepared_wire_size_ = 0;
        prepared_encoding_ = OtaUploadEncoding::Auto;
        status_.method = "idle";
        set_error("wire_size_invalid");
        unlock_status();
        return false;
    }

    status_.partition = http_partition_->label;
    Log::logf(CAT_OTA, LOG_INFO,
              "ESP OTA prepare source=%s partition=%s image_size=%u "
              "encoding=%s wire_size=%u\n",
              source == InstallSource::Url ? "url" : "http_upload",
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
        status_.method = install_source_ == InstallSource::Url ? "url" : "http";
        http_prepared_at_ms_ = millis();
        Log::logf(CAT_OTA, LOG_INFO,
                  "ESP OTA prepared source=%s; AS11 traffic quiesced\n",
                  install_source_ == InstallSource::Url ? "url" : "http_upload");
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
        prepared_encoding_ = OtaUploadEncoding::Auto;
        http_prepared_at_ms_ = 0;
        set_error("as11_quiesce_timeout");
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ESP OTA prepare failed source=%s: AS11 quiesce timeout\n",
                  install_source_ == InstallSource::Url ? "url" : "http_upload");
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
    return begin_upload(filename, image_size, encoding, wire_size,
                        InstallSource::HttpUpload);
}

bool OtaManager::begin_upload(const String &filename,
                              size_t image_size,
                              OtaUploadEncoding encoding,
                              size_t wire_size,
                              InstallSource source) {
    if (wire_size == 0) wire_size = image_size;

    if (!lock_status()) return false;
    if ((source == InstallSource::HttpUpload && status_.url_active) ||
        status_.update_check_active || update_check_task_) {
        set_error("ota_busy");
        unlock_status();
        return false;
    }
    if (status_.http_active) {
        set_error("upload_already_active");
        unlock_status();
        return false;
    }
    if (!status_.http_prepared || prepared_image_size_ != image_size ||
        prepared_encoding_ != encoding || prepared_wire_size_ != wire_size ||
        install_source_ != source) {
        set_error("ota_prepare_required");
        unlock_status();
        return false;
    }

    clear_http_state();
    install_source_ = source;
    status_.method = source == InstallSource::Url ? "url" : "http";
    status_.encoding = ota_upload_encoding_name(encoding);
    status_.http_active = true;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = millis();
    http_encoding_ = encoding;
    http_probe_size_ = 0;
    memset(http_probe_bytes_, 0, sizeof(http_probe_bytes_));
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
    if ((encoding == OtaUploadEncoding::Plain && image_size == 0) ||
        image_size > http_partition_->size) {
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
              "ESP OTA start source=%s file=%s partition=%s image_size=%u "
              "encoding=%s wire_size=%u\n",
              source == InstallSource::Url ? "url" : "http_upload",
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

    if (encoding == OtaUploadEncoding::Auto) {
        return write_auto_http_upload(index, data, len);
    }
    if (encoding == OtaUploadEncoding::Zlib) {
        return write_zlib_http_upload(index, data, len);
    }
    return write_plain_http_upload(index, data, len);
}

bool OtaManager::write_auto_http_upload(size_t index,
                                        const uint8_t *data,
                                        size_t len) {
    if (!lock_status()) return false;
    if (!status_.http_active || !http_partition_ || !http_handle_) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        unlock_status();
        return false;
    }
    if (index != http_probe_size_) {
        unlock_status();
        abort_http_upload("upload_offset_mismatch");
        return false;
    }
    if (!data || len == 0) {
        http_upload_last_activity_ms_ = millis();
        unlock_status();
        return true;
    }

    if (http_probe_size_ == 0 && len >= sizeof(http_probe_bytes_)) {
        uint8_t header[2] = {data[0], data[1]};
        unlock_status();
        if (!resolve_http_upload_encoding(header)) return false;
        return write_http_upload(index, data, len);
    }

    const size_t needed = sizeof(http_probe_bytes_) - http_probe_size_;
    const size_t copied = std::min(needed, len);
    memcpy(http_probe_bytes_ + http_probe_size_, data, copied);
    http_probe_size_ += copied;
    http_upload_last_activity_ms_ = millis();
    const bool ready = http_probe_size_ == sizeof(http_probe_bytes_);
    uint8_t header[2] = {http_probe_bytes_[0], http_probe_bytes_[1]};
    unlock_status();

    if (!ready) return true;
    if (!resolve_http_upload_encoding(header)) return false;
    if (!write_http_upload(0, header, sizeof(header))) return false;
    if (len == copied) return true;
    return write_http_upload(sizeof(header), data + copied, len - copied);
}

bool OtaManager::resolve_http_upload_encoding(const uint8_t header[2]) {
    if (!header) return false;

    OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
    if (header[0] == 0xE9) {
        encoding = OtaUploadEncoding::Plain;
    } else {
        const uint16_t zlib_header =
            static_cast<uint16_t>(header[0]) << 8 | header[1];
        const bool deflate = (header[0] & 0x0F) == 8;
        const bool window_valid = (header[0] >> 4) <= 7;
        const bool checksum_valid = zlib_header % 31 == 0;
        const bool preset_dictionary = (header[1] & 0x20) != 0;
        if (deflate && window_valid && checksum_valid && !preset_dictionary) {
            encoding = OtaUploadEncoding::Zlib;
        }
    }
    if (encoding == OtaUploadEncoding::Auto) {
        abort_http_upload("unsupported_ota_image");
        return false;
    }

    if (!lock_status()) return false;
    if (!status_.http_active || http_encoding_ != OtaUploadEncoding::Auto) {
        unlock_status();
        return false;
    }
    if (encoding == OtaUploadEncoding::Plain) {
        if (status_.total_size &&
            status_.total_size != status_.wire_total_size) {
            unlock_status();
            abort_http_upload("upload_size_mismatch");
            return false;
        }
        status_.total_size = status_.wire_total_size;
    } else if (!begin_zlib_decoder()) {
        unlock_status();
        abort_http_upload("zlib_alloc_failed");
        return false;
    }

    http_encoding_ = encoding;
    status_.encoding = ota_upload_encoding_name(encoding);
    unlock_status();
    return true;
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
        if (!apply_http_progress(chunk) ||
            !apply_http_wire_progress(chunk)) {
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
    if (http_encoding_ == OtaUploadEncoding::Auto) {
        abort_http_upload("image_format_not_detected");
        unlock_status();
        return false;
    }

    const bool zlib = http_encoding_ == OtaUploadEncoding::Zlib;
    if (zlib) {
        unlock_status();
        if (!finish_zlib_http_upload()) return false;
        if (!lock_status()) return false;
    }
    if (status_.bytes == 0 ||
        (status_.total_size && status_.bytes != status_.total_size)) {
        abort_http_upload("incomplete_upload");
        unlock_status();
        return false;
    }
    if (!status_.total_size) status_.total_size = status_.bytes;

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
    prepared_encoding_ = OtaUploadEncoding::Auto;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = 0;
    http_write_in_progress_ = false;
    reset_zlib_decoder();
    status_.progress_percent = 100;
    status_.last_error = "";
    Log::logf(CAT_OTA, LOG_INFO,
              "ESP OTA complete source=%s bytes=%u wire_bytes=%u "
              "encoding=%s partition=%s; rebooting\n",
              install_source_ == InstallSource::Url ? "url" : "http_upload",
              static_cast<unsigned>(status_.bytes),
              static_cast<unsigned>(status_.wire_bytes),
              ota_upload_encoding_name(http_encoding_),
              http_partition_->label);
    http_encoding_ = OtaUploadEncoding::Auto;
    http_handle_ = 0;
    http_partition_ = nullptr;
    schedule_reboot(2000);
    unlock_status();
    return true;
}

void OtaManager::abort_http_upload(const char *reason) {
    abort_upload(reason, true);
}

void OtaManager::abort_upload(const char *reason, bool log_error) {
    if (!lock_status()) return;
    if (http_handle_) esp_ota_abort(http_handle_);
    reset_zlib_decoder();
    http_handle_ = 0;
    http_partition_ = nullptr;
    prepared_image_size_ = 0;
    prepared_wire_size_ = 0;
    prepared_encoding_ = OtaUploadEncoding::Auto;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = 0;
    http_encoding_ = OtaUploadEncoding::Auto;
    http_probe_size_ = 0;
    memset(http_probe_bytes_, 0, sizeof(http_probe_bytes_));
    http_image_magic_checked_ = false;
    http_write_in_progress_ = false;
    status_.http_prepare_pending = false;
    status_.http_prepared = false;
    status_.http_active = false;
    status_.http_ready = false;
    status_.method = "idle";
    status_.partition = "";
    set_error(reason ? reason : "aborted");
    if (log_error) {
        Log::logf(CAT_OTA, LOG_ERROR, "ESP OTA failed source=%s: %s\n",
                  install_source_ == InstallSource::Url ? "url" : "http_upload",
                  status_.last_error.c_str());
    }
    unlock_status();
}

bool OtaManager::request_url_update(const String &url,
                                    OtaUploadEncoding encoding,
                                    size_t image_size,
                                    size_t wire_size) {
    if (!ota_url_supported(url.c_str()) ||
        url.length() > AC_OTA_URL_MAX_LENGTH) {
        if (lock_status()) {
            set_error("url_invalid");
            unlock_status();
        }
        return false;
    }
    if (encoding == OtaUploadEncoding::Plain) {
        if (image_size && wire_size && image_size != wire_size) {
            if (lock_status()) {
                set_error("url_size_mismatch");
                unlock_status();
            }
            return false;
        }
        if (!image_size) image_size = wire_size;
        if (!wire_size) wire_size = image_size;
    }

    char *owned_url = static_cast<char *>(
        Memory::alloc_large(url.length() + 1, false));
    if (!owned_url) {
        if (lock_status()) {
            set_error("url_alloc_failed");
            unlock_status();
        }
        return false;
    }
    memcpy(owned_url, url.c_str(), url.length() + 1);

    if (!lock_status()) {
        Memory::free(owned_url);
        return false;
    }
    if (url_task_ || status_.url_active || update_check_task_ ||
        status_.update_check_active || status_.http_prepare_pending ||
        status_.http_prepared || status_.http_active || status_.http_ready ||
        status_.reboot_pending || arduino_active_) {
        Memory::free(owned_url);
        set_error("ota_busy");
        unlock_status();
        return false;
    }

    clear_http_state();
    install_source_ = InstallSource::Url;
    url_request_ = owned_url;
    url_request_image_size_ = image_size;
    url_request_wire_size_ = wire_size;
    url_request_encoding_ = encoding;
    url_cancel_requested_ = false;
    status_.url_active = true;
    status_.method = "url";
    status_.encoding = ota_upload_encoding_name(encoding);
    status_.total_size = image_size;
    status_.wire_total_size = wire_size;
    status_.last_error = "";

    BaseType_t task_result = xTaskCreatePinnedToCore(
        url_task_entry, "ota_url", AC_OTA_URL_TASK_STACK_BYTES, this,
        AC_OTA_URL_TASK_PRIORITY, &url_task_, AC_OTA_URL_TASK_CORE);
    if (task_result != pdPASS) {
        memset(owned_url, 0, url.length());
        Memory::free(owned_url);
        url_request_ = nullptr;
        url_request_image_size_ = 0;
        url_request_wire_size_ = 0;
        url_request_encoding_ = OtaUploadEncoding::Auto;
        url_task_ = nullptr;
        status_.url_active = false;
        status_.method = "idle";
        set_error("url_task_alloc_failed");
        unlock_status();
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "ESP OTA URL queued encoding=%s image_size=%u wire_size=%u\n",
              ota_upload_encoding_name(encoding),
              static_cast<unsigned>(image_size),
              static_cast<unsigned>(wire_size));
    unlock_status();
    return true;
}

void OtaManager::request_abort(const char *reason) {
    if (!lock_status()) return;
    if (update_check_task_ || status_.update_check_active) {
        update_check_cancel_requested_ = true;
        status_.update_error = reason ? reason : "aborted";
        unlock_status();
        return;
    }
    if (url_task_ || status_.url_active) {
        url_cancel_requested_ = true;
        status_.last_error = reason ? reason : "aborted";
        unlock_status();
        return;
    }
    unlock_status();

    abort_http_upload(reason);
}

void OtaManager::url_task_entry(void *ctx) {
    OtaManager *manager = static_cast<OtaManager *>(ctx);
    if (manager) manager->run_url_task();
    vTaskDelete(nullptr);
}

void OtaManager::run_url_task() {
    char *url = nullptr;
    size_t image_size = 0;
    size_t wire_size = 0;
    OtaUploadEncoding encoding = OtaUploadEncoding::Auto;

    if (lock_status()) {
        url = url_request_;
        image_size = url_request_image_size_;
        wire_size = url_request_wire_size_;
        encoding = url_request_encoding_;
        unlock_status();
    }
    if (!url) {
        fail_url_update("url_request_missing");
        finish_url_task(nullptr);
        return;
    }

    if (wire_size == 0) {
        OtaUrlMetadata metadata;
        OtaUrlError error;
        if (!ota_url_probe(url, metadata, error, url_continue_callback, this)) {
            fail_url_update(error.code, error.http_status, error.esp_error,
                            error.socket_error, error.tls_error,
                            error.tls_flags);
            finish_url_task(url);
            return;
        }
        wire_size = metadata.content_length;
    }

    if (encoding == OtaUploadEncoding::Plain) {
        if (!image_size) image_size = wire_size;
        if (image_size != wire_size) {
            fail_url_update("url_size_mismatch");
            finish_url_task(url);
            return;
        }
    }

    if (!request_upload_prepare(image_size, encoding, wire_size,
                                InstallSource::Url)) {
        const OtaManagerStatus current = status();
        fail_url_update(current.last_error.length()
                            ? current.last_error.c_str()
                            : "url_prepare_failed");
        finish_url_task(url);
        return;
    }

    const uint32_t prepare_started = millis();
    while (true) {
        if (url_cancelled()) {
            fail_url_update("url_cancelled");
            finish_url_task(url);
            return;
        }

        const OtaManagerStatus current = status();
        if (current.http_prepared) break;
        if (!current.http_prepare_pending && current.last_error.length()) {
            fail_url_update(current.last_error.c_str());
            finish_url_task(url);
            return;
        }
        if (static_cast<uint32_t>(millis() - prepare_started) >=
            AC_OTA_URL_PREPARE_TIMEOUT_MS) {
            fail_url_update("url_prepare_timeout");
            finish_url_task(url);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }

    const String filename = "remote_firmware";
    if (!begin_upload(filename, image_size, encoding, wire_size,
                      InstallSource::Url)) {
        const OtaManagerStatus current = status();
        fail_url_update(current.last_error.length()
                            ? current.last_error.c_str()
                            : "url_begin_failed");
        finish_url_task(url);
        return;
    }

    OtaUrlError error;
    if (!ota_url_stream(url, wire_size, url_write_callback,
                        url_continue_callback, this, error)) {
        const OtaManagerStatus current = status();
        if (current.last_error.length()) {
            fail_url_update(current.last_error.c_str());
        } else {
            fail_url_update(error.code, error.http_status, error.esp_error,
                            error.socket_error, error.tls_error,
                            error.tls_flags);
        }
        finish_url_task(url);
        return;
    }

    if (!finish_http_upload()) {
        const OtaManagerStatus current = status();
        fail_url_update(current.last_error.length()
                            ? current.last_error.c_str()
                            : "url_finish_failed");
    }
    finish_url_task(url);
}

void OtaManager::finish_url_task(char *url) {
    if (!lock_status()) return;
    if (url_request_ == url) url_request_ = nullptr;
    url_request_image_size_ = 0;
    url_request_wire_size_ = 0;
    url_request_encoding_ = OtaUploadEncoding::Auto;
    url_cancel_requested_ = false;
    url_task_ = nullptr;
    status_.url_active = false;
    unlock_status();

    if (url) {
        const size_t length = strlen(url);
        memset(url, 0, length);
        Memory::free(url);
    }
}

void OtaManager::fail_url_update(const char *reason,
                                 int http_status,
                                 int esp_error,
                                 int socket_error,
                                 int tls_error,
                                 int tls_flags) {
    abort_upload(reason && *reason ? reason : "url_failed", false);

    Log::logf(CAT_OTA, LOG_ERROR,
              "ESP OTA URL failed: %s http=%d esp=%d socket=%d tls=%d "
              "tls_flags=0x%x\n",
              reason && *reason ? reason : "url_failed", http_status,
              esp_error, socket_error, tls_error,
              static_cast<unsigned>(tls_flags));
}

bool OtaManager::url_cancelled() const {
    if (!lock_status()) return true;
    const bool cancelled = url_cancel_requested_;
    unlock_status();
    return cancelled;
}

bool OtaManager::url_write_callback(void *ctx,
                                    size_t offset,
                                    const uint8_t *data,
                                    size_t len) {
    OtaManager *manager = static_cast<OtaManager *>(ctx);
    return manager && !manager->url_cancelled() &&
           manager->write_http_upload(offset, data, len);
}

bool OtaManager::url_continue_callback(void *ctx) {
    OtaManager *manager = static_cast<OtaManager *>(ctx);
    return manager && !manager->url_cancelled();
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

    const AppConfigData &cfg = *app_config_;

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
    const bool partition_overrun =
        http_partition_ &&
        (status_.bytes > http_partition_->size ||
         bytes > http_partition_->size - status_.bytes);
    const bool declared_overrun =
        status_.total_size &&
        (status_.bytes > status_.total_size ||
         bytes > status_.total_size - status_.bytes);
    if (!status_.http_active || !http_partition_ || partition_overrun ||
        declared_overrun) {
        if (!status_.last_error.length()) {
            set_error(partition_overrun || declared_overrun
                          ? "upload_overrun"
                          : "upload_not_active");
        }
        unlock_status();
        return false;
    }
    status_.bytes += bytes;
    http_upload_last_activity_ms_ = millis();
    if (status_.total_size) {
        status_.progress_percent = static_cast<uint8_t>(
            (status_.bytes * 100ULL) / status_.total_size);
    }
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
        status_.wire_bytes > status_.wire_total_size ||
        bytes > status_.wire_total_size - status_.wire_bytes) {
        if (!status_.last_error.length()) {
            set_error(status_.http_active ? "upload_overrun"
                                          : "upload_not_active");
        }
        unlock_status();
        return false;
    }

    status_.wire_bytes += bytes;
    http_upload_last_activity_ms_ = millis();
    if (!status_.total_size) {
        status_.progress_percent = static_cast<uint8_t>(
            (status_.wire_bytes * 100ULL) / status_.wire_total_size);
    }
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
    prepared_encoding_ = OtaUploadEncoding::Auto;
    http_prepared_at_ms_ = 0;
    http_upload_last_activity_ms_ = 0;
    http_encoding_ = OtaUploadEncoding::Auto;
    http_probe_size_ = 0;
    memset(http_probe_bytes_, 0, sizeof(http_probe_bytes_));
    http_image_magic_checked_ = false;
    http_write_in_progress_ = false;
    status_.http_prepare_pending = false;
    status_.http_prepared = false;
    status_.http_active = false;
    status_.http_ready = false;
    status_.method = "idle";
    status_.encoding = "auto";
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
