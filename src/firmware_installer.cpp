#include "firmware_installer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <miniz.h>

#include "debug_log.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

static constexpr size_t kWriteChunkBytes = 4096;
static constexpr uint32_t kPreparedTtlMs = 60000;
static constexpr uint32_t kWriteIdleTimeoutMs = 60000;

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

const char *firmware_install_source_name(FirmwareInstallSource source) {
    switch (source) {
        case FirmwareInstallSource::HttpUpload: return "http_upload";
        case FirmwareInstallSource::Url: return "url";
        case FirmwareInstallSource::Arduino: return "arduino";
        case FirmwareInstallSource::None:
        default: return "none";
    }
}

void FirmwareInstaller::begin() {
    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();
    esp_ota_mark_app_valid_cancel_rollback();
}

void FirmwareInstaller::poll(bool reboot_allowed) {
    if (!lock()) return;

    const uint32_t now = millis();
    const bool reboot_due =
        reboot_at_ms_ && static_cast<int32_t>(now - reboot_at_ms_) >= 0;
    bool log_reboot_wait = false;
    if (reboot_due && !reboot_allowed && !reboot_wait_logged_) {
        reboot_wait_logged_ = true;
        log_reboot_wait = true;
    }

    if (reboot_due && reboot_allowed) {
        unlock();
        Log::logf(CAT_OTA, LOG_INFO, "rebooting\n");
        delay(50);
        ESP.restart();
        return;
    }

    if (status_.prepared && prepared_at_ms_ &&
        static_cast<int32_t>(now - prepared_at_ms_) >=
            static_cast<int32_t>(kPreparedTtlMs)) {
        const FirmwareInstallSource source = status_.source;

        clear_install_state_locked();
        set_error_locked("ota_prepare_expired");
        Log::logf(CAT_OTA, LOG_WARN,
                  "ESP OTA prepare expired source=%s\n",
                  firmware_install_source_name(source));
    }

    const bool write_timed_out =
        status_.source != FirmwareInstallSource::Arduino &&
        status_.writing && write_last_activity_ms_ &&
        static_cast<int32_t>(now - write_last_activity_ms_) >=
            static_cast<int32_t>(kWriteIdleTimeoutMs);
    unlock();

    if (log_reboot_wait) {
        Log::logf(CAT_OTA, LOG_INFO,
                  "reboot waiting for AS11 quiesce\n");
    }
    if (write_timed_out) abort("upload_timeout");
}

bool FirmwareInstaller::reserve_source(FirmwareInstallSource source,
                                       OtaUploadEncoding encoding,
                                       size_t image_size,
                                       size_t wire_size) {
    if (source == FirmwareInstallSource::None) return false;

    if (!lock()) return false;
    if (status_.source_reserved && status_.source == source) {
        unlock();
        return true;
    }
    if (!source_available_locked()) {
        set_error_locked("ota_busy");
        unlock();
        return false;
    }

    clear_install_state_locked();
    status_.source = source;
    status_.encoding = encoding;
    status_.source_reserved = true;
    status_.total_size = image_size;
    status_.wire_total_size = wire_size;
    status_.last_error = "";
    unlock();
    return true;
}

bool FirmwareInstaller::owned_by(FirmwareInstallSource source) const {
    if (!lock()) return false;
    const bool active = status_.source_reserved || status_.prepare_pending ||
                        status_.prepared || status_.writing || status_.ready ||
                        status_.reboot_pending;
    const bool owned = status_.source == source && active;
    unlock();
    return owned;
}

bool FirmwareInstaller::request_prepare(size_t image_size,
                                        OtaUploadEncoding encoding,
                                        size_t wire_size,
                                        FirmwareInstallSource source) {
    if (source == FirmwareInstallSource::None ||
        source == FirmwareInstallSource::Arduino) {
        return false;
    }
    if (wire_size == 0) wire_size = image_size;

    if (!lock()) return false;

    const bool same_request =
        status_.source == source && prepared_image_size_ == image_size &&
        prepared_encoding_ == encoding && prepared_wire_size_ == wire_size;
    if (same_request && (status_.prepare_pending || status_.prepared)) {
        unlock();
        return true;
    }

    const bool reserved_by_source =
        status_.source_reserved && status_.source == source;
    if (!reserved_by_source && !source_available_locked()) {
        set_error_locked("ota_busy");
        unlock();
        return false;
    }
    if (encoding == OtaUploadEncoding::Zlib && wire_size == 0) {
        set_error_locked("missing_wire_size");
        unlock();
        return false;
    }

    clear_install_state_locked();
    status_.source = source;
    status_.encoding = encoding;
    status_.prepare_pending = true;
    status_.total_size = image_size;
    status_.wire_total_size = wire_size;
    status_.last_error = "";
    prepared_image_size_ = image_size;
    prepared_wire_size_ = wire_size;
    prepared_encoding_ = encoding;

    partition_ = esp_ota_get_next_update_partition(nullptr);
    if (!partition_) {
        clear_install_state_locked();
        set_error_locked("no_ota_partition");
        unlock();
        return false;
    }
    if ((encoding == OtaUploadEncoding::Plain && image_size == 0) ||
        image_size > partition_->size) {
        clear_install_state_locked();
        set_error_locked("image_size_invalid");
        unlock();
        return false;
    }
    if (wire_size == 0) {
        clear_install_state_locked();
        set_error_locked("wire_size_invalid");
        unlock();
        return false;
    }

    status_.partition = partition_->label;
    Log::logf(CAT_OTA, LOG_INFO,
              "ESP OTA prepare source=%s partition=%s image_size=%u "
              "encoding=%s wire_size=%u\n",
              firmware_install_source_name(source), partition_->label,
              static_cast<unsigned>(image_size),
              ota_upload_encoding_name(encoding),
              static_cast<unsigned>(wire_size));
    unlock();
    return true;
}

void FirmwareInstaller::poll_prepare(bool as11_quiesced,
                                     bool as11_quiesce_timed_out) {
    if (!lock()) return;
    if (!status_.prepare_pending || status_.prepared) {
        unlock();
        return;
    }

    if (as11_quiesced) {
        status_.prepare_pending = false;
        status_.prepared = true;
        prepared_at_ms_ = millis();
        Log::logf(CAT_OTA, LOG_INFO,
                  "ESP OTA prepared source=%s; AS11 traffic quiesced\n",
                  firmware_install_source_name(status_.source));
        unlock();
        return;
    }

    if (as11_quiesce_timed_out) {
        const FirmwareInstallSource source = status_.source;

        clear_install_state_locked();
        set_error_locked("as11_quiesce_timeout");
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ESP OTA prepare failed source=%s: AS11 quiesce timeout\n",
                  firmware_install_source_name(source));
    }
    unlock();
}

bool FirmwareInstaller::begin_write(const String &filename,
                                    size_t image_size,
                                    OtaUploadEncoding encoding,
                                    size_t wire_size,
                                    FirmwareInstallSource source) {
    if (wire_size == 0) wire_size = image_size;

    if (!lock()) return false;
    if (!status_.prepared || status_.source != source ||
        prepared_image_size_ != image_size ||
        prepared_encoding_ != encoding ||
        prepared_wire_size_ != wire_size) {
        set_error_locked("ota_prepare_required");
        unlock();
        return false;
    }

    status_.prepared = false;
    status_.writing = true;
    status_.ready = false;
    status_.encoding = encoding;
    status_.bytes = 0;
    status_.total_size = image_size;
    status_.wire_bytes = 0;
    status_.wire_total_size = wire_size;
    status_.progress_percent = 0;
    status_.last_error = "";
    prepared_at_ms_ = 0;
    write_last_activity_ms_ = millis();
    write_encoding_ = encoding;
    probe_size_ = 0;
    memset(probe_bytes_, 0, sizeof(probe_bytes_));
    image_magic_checked_ = false;

    partition_ = esp_ota_get_next_update_partition(nullptr);
    if (!partition_) {
        abort("no_ota_partition");
        unlock();
        return false;
    }
    if ((encoding == OtaUploadEncoding::Plain && image_size == 0) ||
        image_size > partition_->size) {
        abort("image_size_invalid");
        unlock();
        return false;
    }

    status_.partition = partition_->label;
    esp_err_t err = esp_ota_begin(partition_, OTA_WITH_SEQUENTIAL_WRITES,
                                  &ota_handle_);
    if (err != ESP_OK) {
        abort(esp_err_to_name(err));
        unlock();
        return false;
    }
    if (encoding == OtaUploadEncoding::Zlib && !begin_zlib_decoder()) {
        abort("zlib_alloc_failed");
        unlock();
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "ESP OTA start source=%s file=%s partition=%s image_size=%u "
              "encoding=%s wire_size=%u\n",
              firmware_install_source_name(source), filename.c_str(),
              partition_->label, static_cast<unsigned>(image_size),
              ota_upload_encoding_name(encoding),
              static_cast<unsigned>(wire_size));
    unlock();
    return true;
}

bool FirmwareInstaller::write(size_t index, const uint8_t *data, size_t len) {
    if (!lock()) return false;
    const OtaUploadEncoding encoding = write_encoding_;
    unlock();

    if (encoding == OtaUploadEncoding::Auto) {
        return write_auto(index, data, len);
    }
    if (encoding == OtaUploadEncoding::Zlib) {
        return write_zlib(index, data, len);
    }
    return write_plain(index, data, len);
}

bool FirmwareInstaller::write_auto(size_t index,
                                   const uint8_t *data,
                                   size_t len) {
    if (!lock()) return false;
    if (!status_.writing || !partition_ || !ota_handle_) {
        if (!status_.last_error.length()) {
            set_error_locked("upload_not_active");
        }
        unlock();
        return false;
    }
    if (index != probe_size_) {
        unlock();
        abort("upload_offset_mismatch");
        return false;
    }
    if (!data || len == 0) {
        write_last_activity_ms_ = millis();
        unlock();
        return true;
    }

    if (probe_size_ == 0 && len >= sizeof(probe_bytes_)) {
        uint8_t header[2] = {data[0], data[1]};
        unlock();
        if (!resolve_encoding(header)) return false;
        return write(index, data, len);
    }

    const size_t needed = sizeof(probe_bytes_) - probe_size_;
    const size_t copied = std::min(needed, len);
    memcpy(probe_bytes_ + probe_size_, data, copied);
    probe_size_ += copied;
    write_last_activity_ms_ = millis();
    const bool ready = probe_size_ == sizeof(probe_bytes_);
    uint8_t header[2] = {probe_bytes_[0], probe_bytes_[1]};
    unlock();

    if (!ready) return true;
    if (!resolve_encoding(header)) return false;
    if (!write(0, header, sizeof(header))) return false;
    if (len == copied) return true;
    return write(sizeof(header), data + copied, len - copied);
}

bool FirmwareInstaller::resolve_encoding(const uint8_t header[2]) {
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
        abort("unsupported_ota_image");
        return false;
    }

    if (!lock()) return false;
    if (!status_.writing || write_encoding_ != OtaUploadEncoding::Auto) {
        unlock();
        return false;
    }
    if (encoding == OtaUploadEncoding::Plain) {
        if (status_.total_size &&
            status_.total_size != status_.wire_total_size) {
            unlock();
            abort("upload_size_mismatch");
            return false;
        }
        status_.total_size = status_.wire_total_size;
    } else if (!begin_zlib_decoder()) {
        unlock();
        abort("zlib_alloc_failed");
        return false;
    }

    write_encoding_ = encoding;
    status_.encoding = encoding;
    unlock();
    return true;
}

bool FirmwareInstaller::write_plain(size_t index,
                                    const uint8_t *data,
                                    size_t len) {
    if (!lock()) return false;
    if (!status_.writing || !partition_ || !ota_handle_) {
        if (!status_.last_error.length()) {
            set_error_locked("upload_not_active");
        }
        unlock();
        return false;
    }
    if (!data || len == 0) {
        write_last_activity_ms_ = millis();
        unlock();
        return true;
    }

    if (index != status_.bytes) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ESP OTA offset mismatch index=%u expected=%u len=%u "
                  "total=%u\n",
                  static_cast<unsigned>(index),
                  static_cast<unsigned>(status_.bytes),
                  static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.total_size));
        abort("upload_offset_mismatch");
        unlock();
        return false;
    }
    if (status_.total_size == 0 || index > status_.total_size ||
        len > status_.total_size - index) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ESP OTA overrun index=%u len=%u total=%u\n",
                  static_cast<unsigned>(index), static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.total_size));
        abort("upload_overrun");
        unlock();
        return false;
    }
    if (index == 0 && data[0] != 0xE9) {
        abort("bad_esp32_image");
        unlock();
        return false;
    }

    write_last_activity_ms_ = millis();

    const char *write_error = nullptr;
    size_t offset = 0;
    while (offset < len) {
        const size_t chunk = std::min(kWriteChunkBytes, len - offset);
        const esp_err_t err =
            esp_ota_write(ota_handle_, data + offset, chunk);
        if (err != ESP_OK) {
            write_error = esp_err_to_name(err);
            break;
        }
        if (!apply_progress(chunk) || !apply_wire_progress(chunk)) break;
        offset += chunk;
        yield();
    }

    write_last_activity_ms_ = millis();
    if (write_error) {
        abort(write_error);
        unlock();
        return false;
    }

    const bool complete = offset == len;
    unlock();
    return complete;
}

bool FirmwareInstaller::write_zlib(size_t index,
                                   const uint8_t *data,
                                   size_t len) {
    if (!lock()) return false;
    if (!status_.writing || !partition_ || !ota_handle_ || !zlib_decoder_ ||
        !zlib_dict_) {
        if (!status_.last_error.length()) {
            set_error_locked("upload_not_active");
        }
        unlock();
        return false;
    }
    if (!data || len == 0) {
        write_last_activity_ms_ = millis();
        unlock();
        return true;
    }

    if (index != status_.wire_bytes) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ESP OTA zlib offset mismatch index=%u expected=%u "
                  "len=%u total=%u\n",
                  static_cast<unsigned>(index),
                  static_cast<unsigned>(status_.wire_bytes),
                  static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.wire_total_size));
        abort("upload_offset_mismatch");
        unlock();
        return false;
    }
    if (status_.wire_total_size == 0 || index > status_.wire_total_size ||
        len > status_.wire_total_size - index) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ESP OTA zlib overrun index=%u len=%u total=%u\n",
                  static_cast<unsigned>(index), static_cast<unsigned>(len),
                  static_cast<unsigned>(status_.wire_total_size));
        abort("upload_overrun");
        unlock();
        return false;
    }
    if (zlib_finished_) {
        abort("zlib_trailing_data");
        unlock();
        return false;
    }

    tinfl_decompressor *decoder =
        static_cast<tinfl_decompressor *>(zlib_decoder_);
    uint8_t *dict = zlib_dict_;
    const size_t wire_total_size = status_.wire_total_size;
    write_last_activity_ms_ = millis();

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
            !write_decompressed_bytes(dict + out_pos, out_size)) {
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
                      "ESP OTA zlib failed status=%d in=%u/%u out=%u "
                      "total_out=%u\n",
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

    if (done) zlib_finished_ = true;
    write_last_activity_ms_ = millis();

    if (decode_error) {
        abort(decode_error);
        unlock();
        return false;
    }
    if (remaining != 0) {
        abort("zlib_decode_incomplete");
        unlock();
        return false;
    }

    const bool complete = apply_wire_progress(len);
    unlock();
    return complete;
}

bool FirmwareInstaller::finish() {
    if (!lock()) return false;
    if (!status_.writing || !partition_ || !ota_handle_) {
        if (!status_.last_error.length()) {
            set_error_locked("upload_not_active");
        }
        unlock();
        return false;
    }
    if (write_encoding_ == OtaUploadEncoding::Auto) {
        abort("image_format_not_detected");
        unlock();
        return false;
    }

    if (write_encoding_ == OtaUploadEncoding::Zlib) {
        unlock();
        if (!finish_zlib()) return false;
        if (!lock()) return false;
    }
    if (status_.bytes == 0 ||
        (status_.total_size && status_.bytes != status_.total_size)) {
        abort("incomplete_upload");
        unlock();
        return false;
    }
    if (!status_.total_size) status_.total_size = status_.bytes;

    esp_err_t err = esp_ota_end(ota_handle_);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(partition_);
    if (err != ESP_OK) {
        abort(esp_err_to_name(err));
        unlock();
        return false;
    }

    status_.writing = false;
    status_.ready = true;
    status_.progress_percent = 100;
    status_.last_error = "";
    prepared_image_size_ = 0;
    prepared_wire_size_ = 0;
    prepared_encoding_ = OtaUploadEncoding::Auto;
    prepared_at_ms_ = 0;
    write_last_activity_ms_ = 0;
    reset_zlib_decoder();

    Log::logf(CAT_OTA, LOG_INFO,
              "ESP OTA complete source=%s bytes=%u wire_bytes=%u "
              "encoding=%s partition=%s; rebooting\n",
              firmware_install_source_name(status_.source),
              static_cast<unsigned>(status_.bytes),
              static_cast<unsigned>(status_.wire_bytes),
              ota_upload_encoding_name(write_encoding_), partition_->label);

    write_encoding_ = OtaUploadEncoding::Auto;
    ota_handle_ = 0;
    partition_ = nullptr;
    schedule_reboot(2000);
    unlock();
    return true;
}

void FirmwareInstaller::abort(const char *reason, bool log_error) {
    if (!lock()) return;

    const FirmwareInstallSource source = status_.source;
    if (ota_handle_) esp_ota_abort(ota_handle_);
    clear_install_state_locked();
    set_error_locked(reason ? reason : "aborted");

    if (log_error) {
        Log::logf(CAT_OTA, LOG_ERROR, "ESP OTA failed source=%s: %s\n",
                  firmware_install_source_name(source),
                  status_.last_error.c_str());
    }
    unlock();
}

bool FirmwareInstaller::begin_external_install(FirmwareInstallSource source) {
    if (source != FirmwareInstallSource::Arduino) return false;

    if (!lock()) return false;
    if (!source_available_locked()) {
        set_error_locked("ota_busy");
        unlock();
        return false;
    }

    clear_install_state_locked();
    status_.source = source;
    status_.encoding = OtaUploadEncoding::Plain;
    status_.writing = true;
    status_.last_error = "";
    write_last_activity_ms_ = millis();
    unlock();
    return true;
}

void FirmwareInstaller::update_external_progress(size_t bytes,
                                                 size_t total_size) {
    if (!lock()) return;
    if (status_.source != FirmwareInstallSource::Arduino ||
        !status_.writing) {
        unlock();
        return;
    }

    status_.bytes = bytes;
    status_.total_size = total_size;
    status_.wire_bytes = bytes;
    status_.wire_total_size = total_size;
    if (total_size > 0) {
        status_.progress_percent = static_cast<uint8_t>(
            (bytes * 100ULL) / total_size);
    }
    write_last_activity_ms_ = millis();
    unlock();
}

void FirmwareInstaller::complete_external_install() {
    if (!lock()) return;
    if (status_.source != FirmwareInstallSource::Arduino ||
        !status_.writing) {
        unlock();
        return;
    }

    status_.writing = false;
    status_.ready = true;
    status_.progress_percent = 100;
    status_.last_error = "";
    schedule_reboot(2000);
    unlock();
}

void FirmwareInstaller::fail_external_install(const char *reason) {
    abort(reason ? reason : "arduino_ota_failed", false);
}

void FirmwareInstaller::schedule_reboot(uint32_t delay_ms) {
    if (!lock()) return;
    reboot_at_ms_ = millis() + delay_ms;
    reboot_wait_logged_ = false;
    status_.reboot_pending = true;
    unlock();
}

bool FirmwareInstaller::active() const {
    if (!lock()) return false;
    const bool result = status_.source_reserved || status_.prepare_pending ||
                        status_.prepared || status_.writing || status_.ready ||
                        status_.reboot_pending;
    unlock();
    return result;
}

bool FirmwareInstaller::as11_quiesce_required() const {
    if (!lock()) return false;
    const bool required = status_.prepare_pending || status_.prepared ||
                          status_.writing || status_.ready ||
                          status_.reboot_pending;
    unlock();
    return required;
}

FirmwareInstallStatus FirmwareInstaller::status() const {
    if (!lock()) return FirmwareInstallStatus();
    const FirmwareInstallStatus copy = status_;
    unlock();
    return copy;
}

bool FirmwareInstaller::begin_zlib_decoder() {
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

void FirmwareInstaller::reset_zlib_decoder() {
    if (zlib_decoder_) Memory::free(zlib_decoder_);
    if (zlib_dict_) Memory::free(zlib_dict_);

    zlib_decoder_ = nullptr;
    zlib_dict_ = nullptr;
    zlib_output_offset_ = 0;
    zlib_finished_ = false;
}

bool FirmwareInstaller::finish_zlib() {
    if (!lock()) return false;
    if (status_.wire_bytes != status_.wire_total_size) {
        abort("incomplete_wire_upload");
        unlock();
        return false;
    }
    if (!zlib_finished_) {
        abort("zlib_stream_incomplete");
        unlock();
        return false;
    }
    unlock();
    return true;
}

bool FirmwareInstaller::write_decompressed_bytes(const uint8_t *data,
                                                 size_t len) {
    if (!data || len == 0) return true;

    if (!image_magic_checked_) {
        if (data[0] != 0xE9) {
            abort("bad_esp32_image");
            return false;
        }
        image_magic_checked_ = true;
    }

    size_t offset = 0;
    while (offset < len) {
        const size_t chunk = std::min(kWriteChunkBytes, len - offset);
        const esp_err_t err =
            esp_ota_write(ota_handle_, data + offset, chunk);
        if (err != ESP_OK) {
            abort(esp_err_to_name(err));
            return false;
        }
        if (!apply_progress(chunk)) return false;
        offset += chunk;
        yield();
    }

    return true;
}

bool FirmwareInstaller::apply_progress(size_t bytes) {
    if (!lock()) return false;
    const bool partition_overrun =
        partition_ &&
        (status_.bytes > partition_->size ||
         bytes > partition_->size - status_.bytes);
    const bool declared_overrun =
        status_.total_size &&
        (status_.bytes > status_.total_size ||
         bytes > status_.total_size - status_.bytes);
    if (!status_.writing || !partition_ || partition_overrun ||
        declared_overrun) {
        if (!status_.last_error.length()) {
            set_error_locked(partition_overrun || declared_overrun
                                 ? "upload_overrun"
                                 : "upload_not_active");
        }
        unlock();
        return false;
    }

    status_.bytes += bytes;
    write_last_activity_ms_ = millis();
    if (status_.total_size) {
        status_.progress_percent = static_cast<uint8_t>(
            (status_.bytes * 100ULL) / status_.total_size);
    }
    if (status_.progress_percent != last_progress_log_percent_ &&
        (status_.progress_percent % 10 == 0 ||
         status_.progress_percent == 100)) {
        last_progress_log_percent_ = status_.progress_percent;
        Log::logf(CAT_OTA, LOG_DEBUG, "ESP OTA %u%%\n",
                  static_cast<unsigned>(status_.progress_percent));
    }
    unlock();
    return true;
}

bool FirmwareInstaller::apply_wire_progress(size_t bytes) {
    if (!lock()) return false;
    if (!status_.writing || status_.wire_total_size == 0 ||
        status_.wire_bytes > status_.wire_total_size ||
        bytes > status_.wire_total_size - status_.wire_bytes) {
        if (!status_.last_error.length()) {
            set_error_locked(status_.writing ? "upload_overrun"
                                             : "upload_not_active");
        }
        unlock();
        return false;
    }

    status_.wire_bytes += bytes;
    write_last_activity_ms_ = millis();
    if (!status_.total_size) {
        status_.progress_percent = static_cast<uint8_t>(
            (status_.wire_bytes * 100ULL) / status_.wire_total_size);
    }
    unlock();
    return true;
}

bool FirmwareInstaller::source_available_locked() const {
    const bool busy = status_.source_reserved || status_.prepare_pending ||
                      status_.prepared || status_.writing || status_.ready ||
                      status_.reboot_pending;
    return !busy;
}

void FirmwareInstaller::set_error_locked(const char *error) {
    status_.last_error = error ? error : "error";
}

void FirmwareInstaller::clear_install_state_locked() {
    reset_zlib_decoder();
    ota_handle_ = 0;
    partition_ = nullptr;
    prepared_image_size_ = 0;
    prepared_wire_size_ = 0;
    prepared_encoding_ = OtaUploadEncoding::Auto;
    prepared_at_ms_ = 0;
    write_last_activity_ms_ = 0;
    write_encoding_ = OtaUploadEncoding::Auto;
    probe_size_ = 0;
    memset(probe_bytes_, 0, sizeof(probe_bytes_));
    image_magic_checked_ = false;
    status_.source = FirmwareInstallSource::None;
    status_.encoding = OtaUploadEncoding::Auto;
    status_.source_reserved = false;
    status_.prepare_pending = false;
    status_.prepared = false;
    status_.writing = false;
    status_.ready = false;
    status_.partition = "";
    status_.bytes = 0;
    status_.total_size = 0;
    status_.wire_bytes = 0;
    status_.wire_total_size = 0;
    status_.progress_percent = 0;
    last_progress_log_percent_ = 255;
}

bool FirmwareInstaller::lock(TickType_t timeout) const {
    return !mutex_ || xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
}

void FirmwareInstaller::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
