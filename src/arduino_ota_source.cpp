#include "arduino_ota_source.h"

#include <ArduinoOTA.h>
#include <Update.h>

#include "board_net.h"
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

void ArduinoOtaSource::begin(const AppConfigData &config) {
    config_ = &config;
    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();

    if (!lock()) return;
    status_.auth_enabled = config.ota_password.length() > 0;
    status_.port = AC_ARDUINO_OTA_PORT;
    unlock();
}

void ArduinoOtaSource::poll(const NetworkSnapshot &network,
                            bool service_allowed,
                            bool polling_allowed) {
    if (!config_) return;

    const bool owned_install =
        installer_.owned_by(FirmwareInstallSource::Arduino);
    const bool other_install = installer_.active() && !owned_install;

    if (!network.ipv4_ready || !service_allowed || other_install) {
        if (!active()) stop();
        return;
    }
    if (!polling_allowed) return;

    if (!lock()) return;
    status_.auth_enabled = config_->ota_password.length() > 0;

    const bool start_needed = !status_.active &&
                              (!status_.started || config_dirty_);
    unlock();

    if (start_needed) start();
    if (arduino_ota_) arduino_ota_->handle();
}

void ArduinoOtaSource::mark_config_dirty() {
    if (!lock()) return;
    config_dirty_ = true;
    unlock();
}

void ArduinoOtaSource::request_abort(const char *reason) {
    if (!active()) return;

    Update.abort();
    if (lock()) {
        status_.active = false;
        status_.last_error = reason ? reason : "aborted";
        config_dirty_ = true;
        unlock();
    }
    installer_.fail_external_install(reason ? reason : "aborted");
}

bool ArduinoOtaSource::active() const {
    if (!lock()) return false;
    const bool result = status_.active;
    unlock();
    return result;
}

ArduinoOtaSourceStatus ArduinoOtaSource::status() const {
    if (!lock()) return ArduinoOtaSourceStatus();
    const ArduinoOtaSourceStatus copy = status_;
    unlock();
    return copy;
}

void ArduinoOtaSource::start() {
    if (!config_) return;

    stop();

    arduino_ota_ = new ArduinoOTAClass();
    if (!arduino_ota_) {
        set_error("arduino_ota_alloc_failed");
        return;
    }

    const AppConfigData &config = *config_;
    arduino_ota_->setHostname(config.hostname.c_str());
    arduino_ota_->setPort(AC_ARDUINO_OTA_PORT);
    arduino_ota_->setMdnsEnabled(false);
    arduino_ota_->setRebootOnSuccess(false);
    if (config.ota_password.length()) {
        arduino_ota_->setPassword(config.ota_password.c_str());
    }

    arduino_ota_->onStart([this]() {
        const char *type =
            arduino_ota_->getCommand() == U_FLASH ? "firmware" : "filesystem";

        if (!installer_.begin_external_install(
                FirmwareInstallSource::Arduino)) {
            Update.abort();
            set_error("ota_busy");
            Log::logf(CAT_OTA, LOG_WARN,
                      "ArduinoOTA rejected: another install is active\n");
            return;
        }

        if (lock()) {
            status_.active = true;
            status_.bytes = 0;
            status_.total_size = 0;
            status_.progress_percent = 0;
            status_.last_error = "";
            last_progress_log_percent_ = 255;
            unlock();
        }

        Log::logf(CAT_OTA, LOG_INFO, "ArduinoOTA start %s\n", type);
    });

    arduino_ota_->onEnd([this]() {
        if (lock()) {
            status_.active = false;
            status_.progress_percent = 100;
            unlock();
        }

        installer_.complete_external_install();
        Log::logf(CAT_OTA, LOG_INFO, "ArduinoOTA complete; rebooting\n");
    });

    arduino_ota_->onProgress([this](unsigned int progress,
                                    unsigned int total) {
        if (lock()) {
            status_.bytes = progress;
            status_.total_size = total;
            if (total > 0) {
                status_.progress_percent = static_cast<uint8_t>(
                    (progress * 100ULL) / total);
            }

            if (status_.progress_percent != last_progress_log_percent_ &&
                (status_.progress_percent % 10 == 0 ||
                 status_.progress_percent == 100)) {
                last_progress_log_percent_ = status_.progress_percent;
                Log::logf(CAT_OTA, LOG_DEBUG, "ArduinoOTA %u%%\n",
                          static_cast<unsigned>(status_.progress_percent));
            }
            unlock();
        }

        installer_.update_external_progress(progress, total);
    });

    arduino_ota_->onError([this](ota_error_t error) {
        Update.abort();

        const char *name = arduino_ota_error_name(error);
        if (lock()) {
            status_.active = false;
            status_.last_error = name;
            config_dirty_ = true;
            unlock();
        }

        if (installer_.owned_by(FirmwareInstallSource::Arduino)) {
            installer_.fail_external_install(name);
        }
        Log::logf(CAT_OTA, LOG_ERROR, "ArduinoOTA error: %s\n", name);
    });

    arduino_ota_->begin();

    if (!lock()) return;
    status_.started = true;
    status_.auth_enabled = config.ota_password.length() > 0;
    status_.port = AC_ARDUINO_OTA_PORT;
    config_dirty_ = false;
    unlock();

    Log::logf(CAT_OTA, LOG_INFO,
              "ArduinoOTA ready port=%u auth=%s mdns=off\n",
              static_cast<unsigned>(AC_ARDUINO_OTA_PORT),
              config.ota_password.length() ? "on" : "off");
}

void ArduinoOtaSource::stop() {
    if (arduino_ota_) {
        arduino_ota_->end();
        delete arduino_ota_;
        arduino_ota_ = nullptr;
    }

    if (!lock()) return;
    status_.active = false;
    status_.started = false;
    unlock();
}

void ArduinoOtaSource::set_error(const char *error) {
    if (!lock()) return;
    status_.last_error = error ? error : "error";
    unlock();
}

bool ArduinoOtaSource::lock(TickType_t timeout) const {
    return !mutex_ || xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
}

void ArduinoOtaSource::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
