#include "update_checker.h"

#include <cstring>

#include "board.h"
#include "board_net.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "ota_url_client.h"
#include "version.h"

namespace aircannect {
namespace {

struct UpdateCheckContext {
    UpdateChecker *checker = nullptr;
    uint32_t generation = 0;
};

bool deadline_due(uint32_t now, uint32_t deadline) {
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

void UpdateChecker::begin(const AppConfigData &config) {
    config_ = &config;
    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();

    if (!lock()) return;
    status_.enabled = config.update_url.length() > 0 &&
                      AC_OTA_RELEASE_TARGET[0];
    unlock();
}

void UpdateChecker::poll(const NetworkSnapshot &network,
                         bool check_allowed,
                         bool install_active) {
    bool start = false;
    const uint32_t now = millis();

    if (!lock()) return;

    if (config_dirty_) {
        status_.enabled = config_ && config_->update_url.length() > 0 &&
                          AC_OTA_RELEASE_TARGET[0];
        status_.attempted = false;
        status_.checked = false;
        status_.available = false;
        status_.installable = false;
        status_.version = "";
        status_.error = "";
        clear_artifact_locked();
        status_.pending = manual_requested_;
        network_online_ = false;
        network_since_ms_ = 0;
        next_check_ms_ = 0;
        last_check_ms_ = 0;
        config_dirty_ = false;
    }

    if (!status_.enabled) {
        status_.pending = false;
        manual_requested_ = false;
        unlock();
        return;
    }

    if (!network.ipv4_ready) {
        network_online_ = false;
        network_since_ms_ = 0;
        if (task_) cancel_requested_ = true;
        unlock();
        return;
    }

    if (install_active) {
        if (task_) cancel_requested_ = true;
        unlock();
        return;
    }

    if (!network_online_) {
        network_online_ = true;
        network_since_ms_ = now;
    }

    const bool network_settled =
        static_cast<uint32_t>(now - network_since_ms_) >=
        AC_OTA_UPDATE_INITIAL_DELAY_MS;
    const bool first_check_due = !status_.attempted && network_settled;
    const bool periodic_check_due =
        status_.attempted && deadline_due(now, next_check_ms_);

    if (!status_.pending && !status_.active && !task_ &&
        (first_check_due || periodic_check_due)) {
        status_.pending = true;
    }

    const bool schedule_ready = manual_requested_ || network_settled;
    start = status_.pending && schedule_ready && check_allowed && !task_;
    unlock();

    if (start) start_check();
}

void UpdateChecker::mark_config_dirty() {
    if (!lock()) return;

    config_dirty_ = true;
    config_generation_++;
    if (task_) cancel_requested_ = true;
    unlock();
}

bool UpdateChecker::request_check(bool install_active) {
    if (!lock()) return false;

    if (!config_ || !config_->update_url.length() ||
        !AC_OTA_RELEASE_TARGET[0]) {
        status_.error = "update_check_disabled";
        unlock();
        return false;
    }
    if (install_active || status_.pending || status_.active || task_) {
        status_.error = "ota_busy";
        unlock();
        return false;
    }

    manual_requested_ = true;
    status_.pending = true;
    status_.error = "";
    unlock();
    return true;
}

void UpdateChecker::request_abort(const char *reason) {
    if (!lock()) return;
    if (task_ || status_.active) {
        cancel_requested_ = true;
        status_.error = reason ? reason : "aborted";
    } else if (status_.pending) {
        status_.pending = false;
        manual_requested_ = false;
        status_.error = reason ? reason : "aborted";
    }
    unlock();
}

bool UpdateChecker::copy_available_artifact(
    OtaUpdateArtifact &artifact) const {
    if (!lock()) return false;
    if (!status_.available || !status_.installable ||
        !available_artifact_.url) {
        unlock();
        return false;
    }

    artifact.url = available_artifact_.url;
    artifact.image_size = available_artifact_.image_size;
    artifact.wire_size = available_artifact_.wire_size;
    artifact.encoding = available_artifact_.encoding;
    const bool copied = artifact.url.length() > 0;
    unlock();
    return copied;
}

bool UpdateChecker::active() const {
    if (!lock()) return false;
    const bool result = status_.active || task_;
    unlock();
    return result;
}

UpdateCheckerStatus UpdateChecker::status() const {
    if (!lock()) return UpdateCheckerStatus();
    UpdateCheckerStatus copy = status_;
    if (copy.attempted && last_check_ms_) {
        copy.last_check_age_ms = millis() - last_check_ms_;
    }
    unlock();
    return copy;
}

OtaUpdateNotification UpdateChecker::notification() const {
    OtaUpdateNotification notification;
    if (!lock()) return notification;

    notification.checking = status_.pending || status_.active;
    notification.available = status_.available;
    snprintf(notification.version, sizeof(notification.version), "%s",
             status_.version.c_str());
    unlock();
    return notification;
}

bool UpdateChecker::start_check() {
    if (!config_) return false;

    const String &configured_url = config_->update_url;
    const size_t url_len = configured_url.length();
    char *owned_url = static_cast<char *>(
        Memory::alloc_large(url_len + 1, false));
    if (!owned_url) {
        if (lock()) {
            status_.pending = false;
            status_.attempted = true;
            status_.error = "update_url_alloc_failed";
            last_check_ms_ = millis();
            next_check_ms_ =
                last_check_ms_ + AC_OTA_UPDATE_RETRY_INTERVAL_MS;
            unlock();
        }
        return false;
    }
    memcpy(owned_url, configured_url.c_str(), url_len + 1);

    if (!lock()) {
        Memory::free(owned_url);
        return false;
    }
    if (!status_.enabled || task_) {
        Memory::free(owned_url);
        unlock();
        return false;
    }

    check_url_ = owned_url;
    task_generation_ = config_generation_;
    cancel_requested_ = false;
    manual_requested_ = false;
    status_.pending = false;
    status_.active = true;
    status_.error = "";

    const BaseType_t result = xTaskCreatePinnedToCore(
        task_entry, "ota_check", AC_OTA_URL_TASK_STACK_BYTES, this,
        AC_OTA_URL_TASK_PRIORITY, &task_, AC_OTA_URL_TASK_CORE);
    if (result != pdPASS) {
        check_url_ = nullptr;
        task_ = nullptr;
        status_.active = false;
        status_.attempted = true;
        status_.error = "update_task_alloc_failed";
        last_check_ms_ = millis();
        next_check_ms_ = last_check_ms_ + AC_OTA_UPDATE_RETRY_INTERVAL_MS;
        unlock();
        Memory::free(owned_url);
        return false;
    }

    unlock();
    return true;
}

void UpdateChecker::task_entry(void *ctx) {
    UpdateChecker *checker = static_cast<UpdateChecker *>(ctx);
    if (checker) checker->run_task();
    vTaskDelete(nullptr);
}

void UpdateChecker::run_task() {
    char *url = nullptr;
    uint32_t generation = 0;
    if (lock()) {
        url = check_url_;
        generation = task_generation_;
        unlock();
    }
    if (!url) {
        finish_task(nullptr, generation, nullptr, false, {},
                    "update_request_missing", true);
        return;
    }

    uint8_t *manifest_buffer = static_cast<uint8_t *>(
        Memory::alloc_large(AC_OTA_MANIFEST_MAX_BYTES + 1, false));
    if (!manifest_buffer) {
        finish_task(url, generation, nullptr, false, {},
                    "update_manifest_alloc_failed", true);
        return;
    }

    UpdateCheckContext context;
    context.checker = this;
    context.generation = generation;

    size_t manifest_len = 0;
    OtaUrlError transport_error;
    if (!ota_url_fetch(url, manifest_buffer, AC_OTA_MANIFEST_MAX_BYTES,
                       manifest_len, transport_error, continue_callback,
                       &context)) {
        const bool retry_soon =
            transport_error.http_status == 0 ||
            transport_error.http_status == 408 ||
            transport_error.http_status == 429 ||
            transport_error.http_status >= 500;

        Memory::free(manifest_buffer);
        finish_task(url, generation, nullptr, false, {},
                    transport_error.code, retry_soon, &transport_error);
        return;
    }
    manifest_buffer[manifest_len] = '\0';

    OtaReleaseManifest manifest;
    char manifest_error[AC_OTA_RELEASE_ERROR_MAX] = {};
    if (!ota_parse_release_manifest(
            reinterpret_cast<const char *>(manifest_buffer), manifest_len,
            AC_OTA_RELEASE_TARGET, manifest, manifest_error)) {
        Memory::free(manifest_buffer);
        finish_task(url, generation, nullptr, false, {}, manifest_error,
                    false);
        return;
    }

    bool update_available = false;
    if (!ota_release_is_newer(aircannect_version(), manifest.version,
                              update_available)) {
        Memory::free(manifest_buffer);
        finish_task(url, generation, nullptr, false, {},
                    "current_version_invalid", false);
        return;
    }

    OwnedArtifact artifact;
    if (update_available) {
        artifact.url = static_cast<char *>(
            Memory::alloc_large(AC_OTA_URL_MAX_LENGTH + 1, false));
        if (!artifact.url || !ota_resolve_release_artifact_url(
                url, manifest.preferred_artifact.url, artifact.url,
                AC_OTA_URL_MAX_LENGTH + 1)) {
            if (artifact.url) Memory::free(artifact.url);
            Memory::free(manifest_buffer);
            finish_task(url, generation, nullptr, false, {},
                        "manifest_artifact_url_invalid", false);
            return;
        }

        artifact.image_size = manifest.preferred_artifact.image_size;
        artifact.wire_size = manifest.preferred_artifact.wire_size;
        artifact.encoding = manifest.preferred_artifact.zlib
                                ? OtaUploadEncoding::Zlib
                                : OtaUploadEncoding::Plain;
    }

    Memory::free(manifest_buffer);
    finish_task(url, generation, &manifest, update_available, artifact,
                nullptr, false);
}

void UpdateChecker::finish_task(
    char *url,
    uint32_t generation,
    const OtaReleaseManifest *manifest,
    bool update_available,
    OwnedArtifact artifact,
    const char *error,
    bool retry_soon,
    const OtaUrlError *transport_error) {
    bool publish = false;
    const uint32_t now = millis();

    if (lock()) {
        publish = generation == config_generation_;
        if (check_url_ == url) check_url_ = nullptr;
        task_ = nullptr;
        cancel_requested_ = false;
        status_.active = false;

        if (publish) {
            status_.attempted = true;
            last_check_ms_ = now;
            next_check_ms_ = now +
                (retry_soon ? AC_OTA_UPDATE_RETRY_INTERVAL_MS
                            : AC_OTA_UPDATE_CHECK_INTERVAL_MS);

            if (manifest && (!error || !*error)) {
                status_.checked = true;
                status_.available = update_available;
                status_.version = manifest->version;
                status_.error = "";

                clear_artifact_locked();
                if (update_available && artifact.url) {
                    available_artifact_ = artifact;
                    artifact.url = nullptr;
                    status_.installable = true;
                }
            } else {
                status_.error =
                    error && *error ? error : "update_check_failed";
            }
        }
        unlock();
    }

    if (url) Memory::free(url);
    if (artifact.url) Memory::free(artifact.url);
    if (!publish) return;

    if (manifest && (!error || !*error)) {
        if (update_available) {
            Log::logf(CAT_OTA, LOG_INFO,
                      "firmware update available current=%s release=%s\n",
                      aircannect_version(), manifest->version);
        } else {
            Log::logf(CAT_OTA, LOG_DEBUG,
                      "firmware update check current=%s latest=%s\n",
                      aircannect_version(), manifest->version);
        }
        return;
    }

    if (transport_error) {
        Log::logf(CAT_OTA, LOG_WARN,
                  "firmware update check failed error=%s http=%d esp=%d "
                  "socket=%d tls=%d tls_flags=0x%x\n",
                  error && *error ? error : "update_check_failed",
                  transport_error->http_status, transport_error->esp_error,
                  transport_error->socket_error, transport_error->tls_error,
                  static_cast<unsigned>(transport_error->tls_flags));
    } else {
        Log::logf(CAT_OTA, LOG_WARN,
                  "firmware update check failed error=%s\n",
                  error && *error ? error : "update_check_failed");
    }
}

void UpdateChecker::clear_artifact_locked() {
    if (available_artifact_.url) Memory::free(available_artifact_.url);
    available_artifact_ = {};
    status_.installable = false;
}

bool UpdateChecker::cancelled(uint32_t generation) const {
    if (!lock()) return true;
    const bool result = cancel_requested_ ||
                        generation != config_generation_;
    unlock();
    return result;
}

bool UpdateChecker::continue_callback(void *ctx) {
    UpdateCheckContext *context = static_cast<UpdateCheckContext *>(ctx);
    return context && context->checker &&
           !context->checker->cancelled(context->generation);
}

bool UpdateChecker::lock(TickType_t timeout) const {
    return !mutex_ || xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
}

void UpdateChecker::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
