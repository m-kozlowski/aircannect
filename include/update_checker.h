#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdint.h>

#include "app_config.h"
#include "firmware_installer.h"
#include "ota_release_manifest.h"
#include "runtime_snapshots.h"

namespace aircannect {

struct OtaUpdateArtifact {
    String url;
    size_t image_size = 0;
    size_t wire_size = 0;
    OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
};

struct UpdateCheckerStatus {
    bool enabled = false;
    bool pending = false;
    bool active = false;
    bool attempted = false;
    bool checked = false;
    bool available = false;
    bool installable = false;
    String version;
    String error;
    uint32_t last_check_age_ms = 0;
};

struct OtaUpdateNotification {
    bool checking = false;
    bool available = false;
    char version[AC_OTA_RELEASE_VERSION_MAX] = {};
};

class UpdateChecker {
public:
    void begin(const AppConfigData &config);
    void poll(const NetworkSnapshot &network,
              bool check_allowed,
              bool install_active);
    void mark_config_dirty();

    bool request_check(bool install_active);
    void request_abort(const char *reason);
    bool copy_available_artifact(OtaUpdateArtifact &artifact) const;

    bool active() const;
    UpdateCheckerStatus status() const;
    OtaUpdateNotification notification() const;

private:
    struct OwnedArtifact {
        char *url = nullptr;
        size_t image_size = 0;
        size_t wire_size = 0;
        OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
    };

    bool start_check();
    static void task_entry(void *ctx);
    void run_task();
    void finish_task(char *url,
                     uint32_t generation,
                     const OtaReleaseManifest *manifest,
                     bool update_available,
                     OwnedArtifact artifact,
                     const char *error,
                     bool retry_soon,
                     const struct OtaUrlError *transport_error = nullptr);

    void clear_artifact_locked();
    bool cancelled(uint32_t generation) const;
    static bool continue_callback(void *ctx);
    bool lock(TickType_t timeout = portMAX_DELAY) const;
    void unlock() const;

    const AppConfigData *config_ = nullptr;
    UpdateCheckerStatus status_;
    mutable SemaphoreHandle_t mutex_ = nullptr;

    bool config_dirty_ = false;
    bool network_online_ = false;
    bool manual_requested_ = false;
    bool cancel_requested_ = false;
    uint32_t network_since_ms_ = 0;
    uint32_t next_check_ms_ = 0;
    uint32_t last_check_ms_ = 0;
    uint32_t config_generation_ = 1;
    uint32_t task_generation_ = 0;
    char *check_url_ = nullptr;
    TaskHandle_t task_ = nullptr;
    OwnedArtifact available_artifact_;
};

}  // namespace aircannect
