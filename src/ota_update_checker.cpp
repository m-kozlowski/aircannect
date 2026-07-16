#include "ota_manager.h"

#include <string.h>

#include "board.h"
#include "board_net.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "ota_url_client.h"
#include "version.h"

namespace aircannect {
namespace {

struct OtaUpdateCheckContext {
    OtaManager *manager = nullptr;
    uint32_t generation = 0;
};

bool deadline_due(uint32_t now, uint32_t deadline) {
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

void OtaManager::mark_update_config_dirty() {
    if (!lock_status()) return;

    update_config_dirty_ = true;
    update_config_generation_++;
    if (update_check_task_) update_check_cancel_requested_ = true;

    unlock_status();
}

bool OtaManager::request_update_check() {
    if (!lock_status()) return false;

    if (!app_config_ || !app_config_->data().update_url.length() ||
        !AC_OTA_RELEASE_TARGET[0]) {
        status_.update_error = "update_check_disabled";
        unlock_status();
        return false;
    }
    if (status_.reboot_pending || status_.http_prepare_pending ||
        status_.http_prepared || status_.http_active || status_.http_ready ||
        status_.url_active || url_task_ || arduino_active_ ||
        status_.update_check_pending || status_.update_check_active ||
        update_check_task_) {
        status_.update_error = "ota_busy";
        unlock_status();
        return false;
    }

    update_manual_requested_ = true;
    status_.update_check_pending = true;
    status_.update_error = "";
    unlock_status();
    return true;
}

void OtaManager::poll_update_check(bool network_online, bool check_allowed) {
    bool start_check = false;
    const uint32_t now = millis();

    if (!lock_status()) return;

    if (update_config_dirty_) {
        status_.update_check_enabled =
            app_config_ && app_config_->data().update_url.length() > 0 &&
            AC_OTA_RELEASE_TARGET[0];
        status_.update_check_attempted = false;
        status_.update_checked = false;
        status_.update_available = false;
        status_.update_installable = false;
        status_.update_version = "";
        status_.update_error = "";
        clear_available_update_locked();
        status_.update_check_pending = update_manual_requested_;
        update_network_online_ = false;
        update_network_since_ms_ = 0;
        update_next_check_ms_ = 0;
        update_last_check_ms_ = 0;
        update_config_dirty_ = false;
    }

    if (!status_.update_check_enabled) {
        status_.update_check_pending = false;
        update_manual_requested_ = false;
        unlock_status();
        return;
    }

    if (!network_online) {
        update_network_online_ = false;
        update_network_since_ms_ = 0;
        if (update_check_task_) update_check_cancel_requested_ = true;
        unlock_status();
        return;
    }

    if (!update_network_online_) {
        update_network_online_ = true;
        update_network_since_ms_ = now;
    }

    const bool network_settled =
        static_cast<uint32_t>(now - update_network_since_ms_) >=
        AC_OTA_UPDATE_INITIAL_DELAY_MS;
    const bool first_check_due =
        !status_.update_check_attempted && network_settled;
    const bool periodic_check_due =
        status_.update_check_attempted &&
        deadline_due(now, update_next_check_ms_);

    if (!status_.update_check_pending &&
        !status_.update_check_active &&
        !update_check_task_ &&
        (first_check_due || periodic_check_due)) {
        status_.update_check_pending = true;
    }

    const bool install_busy =
        status_.http_prepare_pending || status_.http_prepared ||
        status_.http_active || status_.http_ready || status_.url_active ||
        url_task_ || arduino_active_ || status_.reboot_pending;
    const bool schedule_ready =
        update_manual_requested_ || network_settled;
    start_check = status_.update_check_pending && schedule_ready &&
                  check_allowed && !install_busy && !update_check_task_;
    unlock_status();

    if (start_check) start_update_check();
}

bool OtaManager::start_update_check() {
    if (!app_config_) return false;

    const String &configured_url = app_config_->data().update_url;
    const size_t url_len = configured_url.length();
    char *owned_url = static_cast<char *>(
        Memory::alloc_large(url_len + 1, false));
    if (!owned_url) {
        if (lock_status()) {
            status_.update_check_pending = false;
            status_.update_check_attempted = true;
            status_.update_error = "update_url_alloc_failed";
            update_last_check_ms_ = millis();
            update_next_check_ms_ =
                update_last_check_ms_ + AC_OTA_UPDATE_RETRY_INTERVAL_MS;
            unlock_status();
        }
        return false;
    }
    memcpy(owned_url, configured_url.c_str(), url_len + 1);

    if (!lock_status()) {
        Memory::free(owned_url);
        return false;
    }
    if (!status_.update_check_enabled || update_check_task_ ||
        status_.http_prepare_pending || status_.http_prepared ||
        status_.http_active || status_.http_ready || status_.url_active ||
        url_task_ || arduino_active_ || status_.reboot_pending) {
        Memory::free(owned_url);
        unlock_status();
        return false;
    }

    update_check_url_ = owned_url;
    update_task_generation_ = update_config_generation_;
    update_check_cancel_requested_ = false;
    update_manual_requested_ = false;
    status_.update_check_pending = false;
    status_.update_check_active = true;
    status_.update_error = "";

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        update_check_task_entry, "ota_check", AC_OTA_URL_TASK_STACK_BYTES,
        this, AC_OTA_URL_TASK_PRIORITY, &update_check_task_,
        AC_OTA_URL_TASK_CORE);
    if (task_result != pdPASS) {
        update_check_url_ = nullptr;
        update_check_task_ = nullptr;
        status_.update_check_active = false;
        status_.update_check_attempted = true;
        status_.update_error = "update_task_alloc_failed";
        update_last_check_ms_ = millis();
        update_next_check_ms_ =
            update_last_check_ms_ + AC_OTA_UPDATE_RETRY_INTERVAL_MS;
        unlock_status();
        Memory::free(owned_url);
        return false;
    }

    unlock_status();
    return true;
}

bool OtaManager::request_available_update() {
    String artifact_url;
    size_t image_size = 0;
    size_t wire_size = 0;
    OtaUploadEncoding encoding = OtaUploadEncoding::Auto;

    if (!lock_status()) return false;
    if (!status_.update_available || !status_.update_installable ||
        !available_update_.url) {
        set_error("update_not_available");
        unlock_status();
        return false;
    }

    artifact_url = available_update_.url;
    image_size = available_update_.image_size;
    wire_size = available_update_.wire_size;
    encoding = available_update_.encoding;
    unlock_status();

    if (!artifact_url.length()) {
        if (lock_status()) {
            set_error("update_url_alloc_failed");
            unlock_status();
        }
        return false;
    }

    return request_url_update(artifact_url, encoding,
                              image_size, wire_size);
}

void OtaManager::update_check_task_entry(void *ctx) {
    OtaManager *manager = static_cast<OtaManager *>(ctx);
    if (manager) manager->run_update_check_task();
    vTaskDelete(nullptr);
}

void OtaManager::run_update_check_task() {
    char *url = nullptr;
    uint32_t generation = 0;
    if (lock_status()) {
        url = update_check_url_;
        generation = update_task_generation_;
        unlock_status();
    }
    if (!url) {
        finish_update_check(nullptr, generation, nullptr, false, {},
                            "update_request_missing", true);
        return;
    }

    uint8_t *manifest_buffer = static_cast<uint8_t *>(
        Memory::alloc_large(AC_OTA_MANIFEST_MAX_BYTES + 1, false));
    if (!manifest_buffer) {
        finish_update_check(url, generation, nullptr, false, {},
                            "update_manifest_alloc_failed", true);
        return;
    }

    OtaUpdateCheckContext context;
    context.manager = this;
    context.generation = generation;

    size_t manifest_len = 0;
    OtaUrlError transport_error;
    if (!ota_url_fetch(url, manifest_buffer, AC_OTA_MANIFEST_MAX_BYTES,
                       manifest_len, transport_error,
                       update_check_continue_callback, &context)) {
        const bool retry_soon =
            transport_error.http_status == 0 ||
            transport_error.http_status == 408 ||
            transport_error.http_status == 429 ||
            transport_error.http_status >= 500;

        Memory::free(manifest_buffer);
        finish_update_check(url, generation, nullptr, false, {},
                            transport_error.code, retry_soon,
                            &transport_error);
        return;
    }
    manifest_buffer[manifest_len] = '\0';

    OtaReleaseManifest manifest;
    char manifest_error[AC_OTA_RELEASE_ERROR_MAX] = {};
    if (!ota_parse_release_manifest(
            reinterpret_cast<const char *>(manifest_buffer), manifest_len,
            AC_OTA_RELEASE_TARGET, manifest, manifest_error)) {
        Memory::free(manifest_buffer);
        finish_update_check(url, generation, nullptr, false, {},
                            manifest_error, false);
        return;
    }

    bool update_available = false;
    if (!ota_release_is_newer(aircannect_version(), manifest.version,
                              update_available)) {
        Memory::free(manifest_buffer);
        finish_update_check(url, generation, nullptr, false, {},
                            "current_version_invalid", false);
        return;
    }

    UpdateArtifact artifact;
    if (update_available) {
        artifact.url = static_cast<char *>(
            Memory::alloc_large(AC_OTA_URL_MAX_LENGTH + 1, false));
        if (!artifact.url || !ota_resolve_release_artifact_url(
                url, manifest.preferred_artifact.url, artifact.url,
                AC_OTA_URL_MAX_LENGTH + 1)) {
            if (artifact.url) Memory::free(artifact.url);
            Memory::free(manifest_buffer);
            finish_update_check(url, generation, nullptr, false, {},
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
    finish_update_check(url, generation, &manifest, update_available, artifact,
                        nullptr, false);
}

void OtaManager::finish_update_check(
    char *url,
    uint32_t generation,
    const OtaReleaseManifest *manifest,
    bool update_available,
    UpdateArtifact artifact,
    const char *error,
    bool retry_soon,
    const OtaUrlError *transport_error) {
    bool publish = false;
    const uint32_t now = millis();

    if (lock_status()) {
        publish = generation == update_config_generation_;
        if (update_check_url_ == url) update_check_url_ = nullptr;
        update_check_task_ = nullptr;
        update_check_cancel_requested_ = false;
        status_.update_check_active = false;

        if (publish) {
            status_.update_check_attempted = true;
            update_last_check_ms_ = now;
            update_next_check_ms_ = now +
                (retry_soon ? AC_OTA_UPDATE_RETRY_INTERVAL_MS
                            : AC_OTA_UPDATE_CHECK_INTERVAL_MS);

            if (manifest && (!error || !*error)) {
                status_.update_checked = true;
                status_.update_available = update_available;
                status_.update_version = manifest->version;
                status_.update_error = "";

                clear_available_update_locked();
                if (update_available && artifact.url) {
                    available_update_ = artifact;
                    artifact.url = nullptr;
                    status_.update_installable = true;
                }
            } else {
                status_.update_error =
                    error && *error ? error : "update_check_failed";
            }
        }
        unlock_status();
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

void OtaManager::clear_available_update_locked() {
    if (available_update_.url) Memory::free(available_update_.url);
    available_update_ = {};
    status_.update_installable = false;
}

bool OtaManager::update_check_cancelled(uint32_t generation) const {
    if (!lock_status()) return true;
    const bool cancelled = update_check_cancel_requested_ ||
                           generation != update_config_generation_;
    unlock_status();
    return cancelled;
}

bool OtaManager::update_check_continue_callback(void *ctx) {
    OtaUpdateCheckContext *context =
        static_cast<OtaUpdateCheckContext *>(ctx);
    return context && context->manager &&
           !context->manager->update_check_cancelled(context->generation);
}

}  // namespace aircannect
