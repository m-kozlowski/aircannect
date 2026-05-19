#include "ota_manager.h"

#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_err.h>
#include <esp_ota_ops.h>

#include "debug_log.h"

namespace aircannect {

namespace {

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

void OtaManager::begin(AppConfig &app_config) {
    app_config_ = &app_config;
    status_.auth_enabled = app_config.data().ota_password.length() > 0;
    status_.arduino_port = AC_ARDUINO_OTA_PORT;
    esp_ota_mark_app_valid_cancel_rollback();
}

void OtaManager::poll(const WifiManager &wifi_manager) {
    if (reboot_at_ms_ &&
        static_cast<int32_t>(millis() - reboot_at_ms_) >= 0) {
        Log::logf(CAT_OTA, LOG_INFO, "[OTA] rebooting\n");
        delay(50);
        ESP.restart();
    }

    if (!app_config_) return;

    status_.auth_enabled = app_config_->data().ota_password.length() > 0;
    if (!wifi_manager.network_available()) {
        if (status_.arduino_started) stop_arduino_ota();
        return;
    }

    if (!status_.arduino_started || arduino_config_dirty_) {
        start_arduino_ota();
    }
    if (arduino_ota_) arduino_ota_->handle();
}

void OtaManager::mark_config_dirty() {
    arduino_config_dirty_ = true;
}

bool OtaManager::begin_http_upload(const String &filename, size_t image_size) {
    if (status_.http_active) {
        set_error("upload_already_active");
        return false;
    }
    clear_http_state();
    status_.method = "http";
    status_.http_active = true;
    status_.bytes = 0;
    status_.total_size = image_size;
    status_.progress_percent = 0;
    status_.partition = "";
    status_.last_error = "";

    http_partition_ = esp_ota_get_next_update_partition(nullptr);
    if (!http_partition_) {
        abort_http_upload("no_ota_partition");
        return false;
    }
    if (image_size == 0 || image_size > http_partition_->size) {
        abort_http_upload("image_size_invalid");
        return false;
    }

    status_.partition = http_partition_->label;
    esp_err_t err = esp_ota_begin(http_partition_, image_size, &http_handle_);
    if (err != ESP_OK) {
        abort_http_upload(esp_err_to_name(err));
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "[OTA] HTTP upload start file=%s partition=%s image_size=%u\n",
              filename.c_str(), http_partition_->label,
              static_cast<unsigned>(image_size));
    return true;
}

bool OtaManager::write_http_upload(const uint8_t *data, size_t len) {
    if (!status_.http_active || !http_partition_ || !http_handle_) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        return false;
    }
    if (!data || len == 0) return true;

    if (status_.bytes == 0 && data[0] != 0xE9) {
        abort_http_upload("bad_esp32_image");
        return false;
    }
    if (status_.total_size == 0 || status_.bytes + len > status_.total_size) {
        abort_http_upload("image_too_large");
        return false;
    }

    esp_err_t err = esp_ota_write(http_handle_, data, len);
    if (err != ESP_OK) {
        abort_http_upload(esp_err_to_name(err));
        return false;
    }

    status_.bytes += len;
    status_.progress_percent =
        static_cast<uint8_t>((status_.bytes * 100ULL) / status_.total_size);
    if (status_.progress_percent != last_progress_log_percent_ &&
        (status_.progress_percent % 10 == 0 ||
         status_.progress_percent == 100)) {
        last_progress_log_percent_ = status_.progress_percent;
        Log::logf(CAT_OTA, LOG_DEBUG, "[OTA] HTTP upload %u%%\n",
                  static_cast<unsigned>(status_.progress_percent));
    }
    return true;
}

bool OtaManager::finish_http_upload() {
    if (!status_.http_active || !http_partition_ || !http_handle_) {
        if (!status_.last_error.length()) set_error("upload_not_active");
        return false;
    }
    if (status_.total_size == 0 || status_.bytes != status_.total_size) {
        abort_http_upload("incomplete_upload");
        return false;
    }

    esp_err_t err = esp_ota_end(http_handle_);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(http_partition_);
    }

    if (err != ESP_OK) {
        abort_http_upload(esp_err_to_name(err));
        return false;
    }

    status_.http_active = false;
    status_.http_ready = true;
    status_.progress_percent = 100;
    status_.last_error = "";
    Log::logf(CAT_OTA, LOG_INFO,
              "[OTA] HTTP upload complete bytes=%u partition=%s; rebooting\n",
              static_cast<unsigned>(status_.bytes),
              http_partition_->label);
    http_handle_ = 0;
    http_partition_ = nullptr;
    schedule_reboot(2000);
    return true;
}

void OtaManager::abort_http_upload(const char *reason) {
    if (http_handle_) esp_ota_abort(http_handle_);
    http_handle_ = 0;
    http_partition_ = nullptr;
    status_.http_active = false;
    status_.http_ready = false;
    set_error(reason ? reason : "aborted");
    Log::logf(CAT_OTA, LOG_ERROR, "[OTA] HTTP upload failed: %s\n",
              status_.last_error.c_str());
}

void OtaManager::schedule_reboot(uint32_t delay_ms) {
    reboot_at_ms_ = millis() + delay_ms;
    status_.reboot_pending = true;
}

void OtaManager::start_arduino_ota() {
    if (!app_config_) return;
    stop_arduino_ota();

    arduino_ota_ = new ArduinoOTAClass();
    if (!arduino_ota_) {
        set_error("arduino_ota_alloc_failed");
        return;
    }

    const AppConfigData &cfg = app_config_->data();
    arduino_ota_->setHostname(cfg.hostname.c_str());
    arduino_ota_->setPort(AC_ARDUINO_OTA_PORT);
    arduino_ota_->setMdnsEnabled(false);
    arduino_ota_->setRebootOnSuccess(true);
    if (cfg.ota_password.length()) {
        arduino_ota_->setPassword(cfg.ota_password.c_str());
    }

    arduino_ota_->onStart([this]() {
        arduino_active_ = true;
        status_.method = "arduino";
        status_.bytes = 0;
        status_.total_size = 0;
        status_.progress_percent = 0;
        status_.last_error = "";
        last_progress_log_percent_ = 255;
        const char *type =
            arduino_ota_->getCommand() == U_FLASH ? "firmware" : "filesystem";
        Log::logf(CAT_OTA, LOG_INFO, "[OTA] ArduinoOTA start %s\n", type);
    });
    arduino_ota_->onEnd([this]() {
        arduino_active_ = false;
        status_.progress_percent = 100;
        Log::logf(CAT_OTA, LOG_INFO,
                  "[OTA] ArduinoOTA complete; rebooting\n");
    });
    arduino_ota_->onProgress([this](unsigned int progress,
                                    unsigned int total) {
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
            Log::logf(CAT_OTA, LOG_DEBUG, "[OTA] ArduinoOTA %u%%\n",
                      static_cast<unsigned>(status_.progress_percent));
        }
    });
    arduino_ota_->onError([this](ota_error_t error) {
        arduino_active_ = false;
        Update.abort();
        set_error(arduino_ota_error_name(error));
        arduino_config_dirty_ = true;
        Log::logf(CAT_OTA, LOG_ERROR, "[OTA] ArduinoOTA error: %s\n",
                  status_.last_error.c_str());
    });

    arduino_ota_->begin();
    status_.arduino_started = true;
    arduino_config_dirty_ = false;
    Log::logf(CAT_OTA, LOG_INFO,
              "[OTA] ArduinoOTA ready port=%u auth=%s mdns=off\n",
              static_cast<unsigned>(AC_ARDUINO_OTA_PORT),
              cfg.ota_password.length() ? "on" : "off");
}

void OtaManager::stop_arduino_ota() {
    if (arduino_ota_) {
        arduino_ota_->end();
        delete arduino_ota_;
        arduino_ota_ = nullptr;
    }
    arduino_active_ = false;
    status_.arduino_started = false;
}

void OtaManager::set_error(const char *error) {
    status_.last_error = error ? error : "error";
}

void OtaManager::clear_http_state() {
    http_handle_ = 0;
    http_partition_ = nullptr;
    status_.http_active = false;
    status_.http_ready = false;
    status_.bytes = 0;
    status_.total_size = 0;
    status_.progress_percent = 0;
    last_progress_log_percent_ = 255;
}

}  // namespace aircannect
