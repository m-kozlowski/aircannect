#include "firmware_url_source.h"

#include <cstring>

#include "board_net.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "ota_url_client.h"

namespace aircannect {

void FirmwareUrlSource::begin() {
    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();
}

bool FirmwareUrlSource::request(const String &url,
                                OtaUploadEncoding encoding,
                                size_t image_size,
                                size_t wire_size) {
    if (!ota_url_supported(url.c_str()) ||
        url.length() > AC_OTA_URL_MAX_LENGTH) {
        set_error("url_invalid");
        return false;
    }
    if (encoding == OtaUploadEncoding::Plain) {
        if (image_size && wire_size && image_size != wire_size) {
            set_error("url_size_mismatch");
            return false;
        }
        if (!image_size) image_size = wire_size;
        if (!wire_size) wire_size = image_size;
    }

    char *owned_url = static_cast<char *>(
        Memory::alloc_large(url.length() + 1, false));
    if (!owned_url) {
        set_error("url_alloc_failed");
        return false;
    }
    memcpy(owned_url, url.c_str(), url.length() + 1);

    if (!lock()) {
        Memory::free(owned_url);
        return false;
    }
    const bool source_busy = admission_pending_ || task_ || status_.active;
    if (!source_busy) admission_pending_ = true;
    unlock();
    if (source_busy) {
        Memory::free(owned_url);
        set_error("ota_busy");
        return false;
    }

    if (!installer_.reserve_source(FirmwareInstallSource::Url, encoding,
                                   image_size, wire_size)) {
        Memory::free(owned_url);
        const FirmwareInstallStatus install = installer_.status();
        if (lock()) {
            admission_pending_ = false;
            status_.last_error = install.last_error.length()
                                     ? install.last_error
                                     : "ota_busy";
            unlock();
        }
        return false;
    }

    if (!lock()) {
        installer_.abort("url_source_lock_failed", false);
        Memory::free(owned_url);
        return false;
    }
    if (task_ || status_.active) {
        admission_pending_ = false;
        unlock();
        installer_.abort("ota_busy", false);
        Memory::free(owned_url);
        set_error("ota_busy");
        return false;
    }

    request_url_ = owned_url;
    request_image_size_ = image_size;
    request_wire_size_ = wire_size;
    request_encoding_ = encoding;
    admission_pending_ = false;
    cancel_requested_ = false;
    status_.active = true;
    status_.last_error = "";

    const BaseType_t result = xTaskCreatePinnedToCore(
        task_entry, "ota_url", AC_OTA_URL_TASK_STACK_BYTES, this,
        AC_OTA_URL_TASK_PRIORITY, &task_, AC_OTA_URL_TASK_CORE);
    if (result != pdPASS) {
        request_url_ = nullptr;
        request_image_size_ = 0;
        request_wire_size_ = 0;
        request_encoding_ = OtaUploadEncoding::Auto;
        admission_pending_ = false;
        task_ = nullptr;
        status_.active = false;
        status_.last_error = "url_task_alloc_failed";
        unlock();

        memset(owned_url, 0, url.length());
        Memory::free(owned_url);
        installer_.abort("url_task_alloc_failed", false);
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "ESP OTA URL queued encoding=%s image_size=%u wire_size=%u\n",
              ota_upload_encoding_name(encoding),
              static_cast<unsigned>(image_size),
              static_cast<unsigned>(wire_size));
    unlock();
    return true;
}

void FirmwareUrlSource::request_abort(const char *reason) {
    if (!lock()) return;
    if (task_ || status_.active) {
        cancel_requested_ = true;
        status_.last_error = reason ? reason : "aborted";
        unlock();
        return;
    }
    unlock();

    if (installer_.owned_by(FirmwareInstallSource::Url)) {
        installer_.abort(reason ? reason : "aborted");
    }
}

bool FirmwareUrlSource::active() const {
    if (!lock()) return false;
    const bool result = admission_pending_ || task_ || status_.active;
    unlock();
    return result;
}

FirmwareUrlSourceStatus FirmwareUrlSource::status() const {
    if (!lock()) return FirmwareUrlSourceStatus();
    const FirmwareUrlSourceStatus copy = status_;
    unlock();
    return copy;
}

void FirmwareUrlSource::task_entry(void *ctx) {
    FirmwareUrlSource *source = static_cast<FirmwareUrlSource *>(ctx);
    if (source) source->run_task();
    vTaskDelete(nullptr);
}

void FirmwareUrlSource::run_task() {
    char *url = nullptr;
    size_t image_size = 0;
    size_t wire_size = 0;
    OtaUploadEncoding encoding = OtaUploadEncoding::Auto;

    if (lock()) {
        url = request_url_;
        image_size = request_image_size_;
        wire_size = request_wire_size_;
        encoding = request_encoding_;
        unlock();
    }
    if (!url) {
        fail("url_request_missing");
        finish_task(nullptr);
        return;
    }

    if (wire_size == 0) {
        OtaUrlMetadata metadata;
        OtaUrlError error;
        if (!ota_url_probe(url, metadata, error, continue_callback, this)) {
            fail(error.code, error.http_status, error.esp_error,
                 error.socket_error, error.tls_error, error.tls_flags);
            finish_task(url);
            return;
        }
        wire_size = metadata.content_length;
    }

    if (encoding == OtaUploadEncoding::Plain) {
        if (!image_size) image_size = wire_size;
        if (image_size != wire_size) {
            fail("url_size_mismatch");
            finish_task(url);
            return;
        }
    }

    if (!installer_.request_prepare(image_size, encoding, wire_size,
                                    FirmwareInstallSource::Url)) {
        const FirmwareInstallStatus current = installer_.status();
        fail(current.last_error.length() ? current.last_error.c_str()
                                         : "url_prepare_failed");
        finish_task(url);
        return;
    }

    const uint32_t prepare_started = millis();
    while (true) {
        if (cancelled()) {
            fail("url_cancelled");
            finish_task(url);
            return;
        }

        const FirmwareInstallStatus current = installer_.status();
        if (current.prepared) break;
        if (!current.prepare_pending && current.last_error.length()) {
            fail(current.last_error.c_str());
            finish_task(url);
            return;
        }
        if (static_cast<uint32_t>(millis() - prepare_started) >=
            AC_OTA_URL_PREPARE_TIMEOUT_MS) {
            fail("url_prepare_timeout");
            finish_task(url);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (!installer_.begin_write("remote_firmware", image_size, encoding,
                                wire_size, FirmwareInstallSource::Url)) {
        const FirmwareInstallStatus current = installer_.status();
        fail(current.last_error.length() ? current.last_error.c_str()
                                         : "url_begin_failed");
        finish_task(url);
        return;
    }

    OtaUrlError error;
    if (!ota_url_stream(url, wire_size, write_callback, continue_callback,
                        this, error)) {
        const FirmwareInstallStatus current = installer_.status();
        if (current.last_error.length()) {
            fail(current.last_error.c_str());
        } else {
            fail(error.code, error.http_status, error.esp_error,
                 error.socket_error, error.tls_error, error.tls_flags);
        }
        finish_task(url);
        return;
    }

    if (!installer_.finish()) {
        const FirmwareInstallStatus current = installer_.status();
        fail(current.last_error.length() ? current.last_error.c_str()
                                         : "url_finish_failed");
    }
    finish_task(url);
}

void FirmwareUrlSource::finish_task(char *url) {
    if (!lock()) return;
    if (request_url_ == url) request_url_ = nullptr;
    request_image_size_ = 0;
    request_wire_size_ = 0;
    request_encoding_ = OtaUploadEncoding::Auto;
    cancel_requested_ = false;
    task_ = nullptr;
    status_.active = false;
    unlock();

    if (url) {
        const size_t length = strlen(url);
        memset(url, 0, length);
        Memory::free(url);
    }
}

void FirmwareUrlSource::fail(const char *reason,
                             int http_status,
                             int esp_error,
                             int socket_error,
                             int tls_error,
                             int tls_flags) {
    const char *error = reason && *reason ? reason : "url_failed";
    installer_.abort(error, false);
    set_error(error);

    Log::logf(CAT_OTA, LOG_ERROR,
              "ESP OTA URL failed: %s http=%d esp=%d socket=%d tls=%d "
              "tls_flags=0x%x\n",
              error, http_status, esp_error, socket_error, tls_error,
              static_cast<unsigned>(tls_flags));
}

bool FirmwareUrlSource::cancelled() const {
    if (!lock()) return true;
    const bool result = cancel_requested_;
    unlock();
    return result;
}

void FirmwareUrlSource::set_error(const char *error) {
    if (!lock()) return;
    status_.last_error = error ? error : "error";
    unlock();
}

bool FirmwareUrlSource::write_callback(void *ctx,
                                       size_t offset,
                                       const uint8_t *data,
                                       size_t len) {
    FirmwareUrlSource *source = static_cast<FirmwareUrlSource *>(ctx);
    return source && !source->cancelled() &&
           source->installer_.write(offset, data, len);
}

bool FirmwareUrlSource::continue_callback(void *ctx) {
    FirmwareUrlSource *source = static_cast<FirmwareUrlSource *>(ctx);
    return source && !source->cancelled();
}

bool FirmwareUrlSource::lock(TickType_t timeout) const {
    return !mutex_ || xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
}

void FirmwareUrlSource::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
