#include "update_checker.h"

#include <cstring>
#include <freertos/idf_additions.h>

#include "board.h"
#include "board_net.h"
#include "debug_log.h"
#include "export_task.h"
#include "memory_manager.h"
#include "ota_url_client.h"
#include "tls_memory.h"
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

static constexpr uint32_t UPDATE_HEAP_RETRY_MS = 10 * 1000;

}  // namespace

void UpdateChecker::begin(const AppConfigData &config, ExportTask &exports) {
    config_ = &config;
    exports_ = &exports;
    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();

    if (!lock()) return;
    status_.enabled = config.update_url.length() > 0 &&
                      AC_OTA_RELEASE_TARGET[0];
    unlock();
}

void UpdateChecker::poll(const NetworkSnapshot &network,
                         bool check_allowed,
                         bool install_active,
                         bool therapy_active,
                         const char *blocked_reason,
                         bool automatic_check_allowed) {
    bool start = false;
    bool manual = false;
    bool log_heap_deferred = false;
    const uint32_t now = millis();

    if (!lock()) return;

    reap_task_locked();
    if (status_.active) trace_phase(trace_phase_);

    if (config_dirty_) {
        status_.enabled = config_ && config_->update_url.length() > 0 &&
                          AC_OTA_RELEASE_TARGET[0];
        status_.attempted = false;
        status_.checked = false;
        status_.available = false;
        status_.installable = false;
        status_.version = "";
        status_.error = "";
        clear_release_locked();
        status_.pending = manual_requested_;
        network_online_ = false;
        network_since_ms_ = 0;
        next_check_ms_ = 0;
        heap_retry_at_ms_ = 0;
        last_check_ms_ = 0;
        config_dirty_ = false;
    }

    if (!status_.enabled) {
        status_.pending = false;
        manual_requested_ = false;
        unlock();
        return;
    }

    if (therapy_active && task_ && check_url_) {
        therapy_preempted_ = true;
    }

    if (!network.ipv4_ready) {
        if (status_.pending) trace_phase("waiting_network");
        network_online_ = false;
        network_since_ms_ = 0;
        if (task_) cancel_requested_ = true;
        unlock();
        return;
    }

    if (install_active) {
        if (status_.pending) trace_phase("waiting_install");
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
        begin_trace(first_check_due ? "startup" : "periodic");
    }

    const bool schedule_ready = manual_requested_ || network_settled;
    const bool automatic_ready = manual_requested_ || automatic_check_allowed;
    log_heap_deferred = heap_retry_at_ms_ == 0;
    const bool heap_retry_ready = heap_retry_at_ms_ == 0 ||
        deadline_due(now, heap_retry_at_ms_);
    start = status_.pending && schedule_ready && check_allowed &&
        !therapy_active && !task_ && heap_retry_ready && automatic_ready;
    manual = manual_requested_;
    if (status_.pending && !start) {
        trace_phase(therapy_active ? "waiting_therapy" :
                    !check_allowed ? blocked_reason :
                    !automatic_ready ? "waiting_post_therapy" :
                    !schedule_ready ? "waiting_network_settle" :
                    !heap_retry_ready ? "waiting_tls_heap" :
                    "waiting_task_cleanup");
    }
    unlock();

    if (!start) return;

    if (!exports_->try_begin_update_check(manual)) {
        trace_phase("waiting_export_reservation");
        return;
    }

    if (!tls_heap_available(log_heap_deferred)) {
        exports_->end_update_check();
        trace_phase("waiting_tls_heap");
        if (lock()) {
            heap_retry_at_ms_ = millis() + UPDATE_HEAP_RETRY_MS;
            unlock();
        }
        return;
    }

    if (lock()) {
        heap_retry_at_ms_ = 0;
        unlock();
    }
    if (!start_check()) exports_->end_update_check();
}

void UpdateChecker::mark_config_dirty() {
    if (!lock()) return;

    config_dirty_ = true;
    config_generation_++;
    if (task_) {
        cancel_requested_ = true;
        manual_requested_ = false;
    }
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
    if (!tls_heap_available(false)) {
        status_.error = "tls_heap_guard";
        unlock();
        return false;
    }

    manual_requested_ = true;
    status_.pending = true;
    status_.error = "";
    begin_trace("manual");
    unlock();
    return true;
}

void UpdateChecker::request_abort(const char *reason) {
    if (!lock()) return;
    if ((task_ || status_.active) && check_url_) {
        cancel_requested_ = true;
        status_.pending = false;
        manual_requested_ = false;
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

std::shared_ptr<const LargeByteBuffer> UpdateChecker::release_notes() const {
    if (!lock()) return {};

    std::shared_ptr<const LargeByteBuffer> notes;
    if (status_.available && status_.release_notes_available) {
        notes = release_notes_;
    }
    unlock();
    return notes;
}

bool UpdateChecker::active() const {
    if (!lock()) return false;
    const bool result = status_.active || task_;
    unlock();
    return result;
}

bool UpdateChecker::manual_check_active() const {
    if (!lock()) return false;

    const bool result = task_ && task_manual_;
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
        trace_phase("failed_url_allocation");
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
    task_manual_ = manual_requested_;
    cancel_requested_ = false;
    export_preempted_ = false;
    therapy_preempted_ = false;
    status_.pending = false;
    status_.active = true;
    status_.error = "";
    trace_phase("task_dispatch");

    BaseType_t result = pdFAIL;
    task_stack_external_ = Memory::psram_available();
    if (task_stack_external_) {
        result = xTaskCreatePinnedToCoreWithCaps(
            task_entry, "ota_check", AC_OTA_URL_TASK_STACK_BYTES, this,
            AC_OTA_UPDATE_CHECK_TASK_PRIORITY, &task_,
            AC_OTA_URL_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else {
        result = xTaskCreatePinnedToCore(
            task_entry, "ota_check", AC_OTA_URL_TASK_STACK_BYTES, this,
            AC_OTA_UPDATE_CHECK_TASK_PRIORITY, &task_,
            AC_OTA_URL_TASK_CORE);
    }
    if (result != pdPASS) {
        trace_phase("failed_task_allocation");
        check_url_ = nullptr;
        task_ = nullptr;
        manual_requested_ = false;
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

bool UpdateChecker::tls_heap_available(bool log_deferred) const {
    const TlsMemoryAdmission admission = TlsMemory::admission();
    if (admission.available) return true;

    if (log_deferred) {
        Log::logf(CAT_OTA, LOG_DEBUG,
                  "firmware update check deferred: TLS heap guard "
                  "free=%u max_alloc=%u min_free=%u min_max_alloc=%u\n",
                  static_cast<unsigned>(admission.internal_free),
                  static_cast<unsigned>(admission.internal_largest),
                  static_cast<unsigned>(admission.minimum_free),
                  static_cast<unsigned>(admission.minimum_largest));
    }
    return false;
}

void UpdateChecker::task_entry(void *ctx) {
    UpdateChecker *checker = static_cast<UpdateChecker *>(ctx);
    checker->run_task();

    // The main loop reaps this task before allowing an export to start.
    vTaskSuspend(nullptr);
}

void UpdateChecker::reap_task_locked() {
    if (!task_ || eTaskGetState(task_) != eSuspended) return;

    if (task_stack_external_) {
        vTaskDeleteWithCaps(task_);
    } else {
        vTaskDelete(task_);
    }
    task_ = nullptr;
    task_manual_ = false;
    status_.active = false;
    cancel_requested_ = false;
    exports_->end_update_check();
}

void UpdateChecker::run_task() {
#if AC_STACK_PROFILE_ENABLED
    TlsMemory::Measurement tls_measurement;
    tls_measurement.start("ota_check");
#endif

    trace_phase("worker_started");
    char *url = nullptr;
    uint32_t generation = 0;
    if (lock()) {
        url = check_url_;
        generation = task_generation_;
        unlock();
    }
    if (!url) {
        finish_task(nullptr, generation, nullptr, false, {}, {},
                    "update_request_missing", true);
        return;
    }

    uint8_t *manifest_buffer = static_cast<uint8_t *>(
        Memory::alloc_large(AC_OTA_MANIFEST_MAX_BYTES + 1, false));
    if (!manifest_buffer) {
        finish_task(url, generation, nullptr, false, {}, {},
                    "update_manifest_alloc_failed", true);
        return;
    }

    UpdateCheckContext context;
    context.checker = this;
    context.generation = generation;

    size_t manifest_len = 0;
    OtaUrlError transport_error;
    trace_phase("manifest_fetch");
    if (!ota_url_fetch(url, manifest_buffer, AC_OTA_MANIFEST_MAX_BYTES,
                       manifest_len, transport_error, continue_callback,
                       &context, trace_callback)) {
        const bool retry_soon =
            transport_error.http_status == 0 ||
            transport_error.http_status == 408 ||
            transport_error.http_status == 429 ||
            transport_error.http_status >= 500;

        Memory::free(manifest_buffer);
        finish_task(url, generation, nullptr, false, {}, {},
                    transport_error.code, retry_soon, &transport_error);
        return;
    }
    manifest_buffer[manifest_len] = '\0';
    trace_phase("manifest_parse");

    OtaReleaseManifest manifest;
    char manifest_error[AC_OTA_RELEASE_ERROR_MAX] = {};
    if (!ota_parse_release_manifest(
            reinterpret_cast<const char *>(manifest_buffer), manifest_len,
            AC_OTA_RELEASE_TARGET, manifest, manifest_error)) {
        Memory::free(manifest_buffer);
        finish_task(url, generation, nullptr, false, {}, {}, manifest_error,
                    false);
        return;
    }

    bool update_available = false;
    if (!ota_release_is_newer(aircannect_version(), manifest.version,
                              update_available)) {
        Memory::free(manifest_buffer);
        finish_task(url, generation, nullptr, false, {}, {},
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
            finish_task(url, generation, nullptr, false, {}, {},
                        "manifest_artifact_url_invalid", false);
            return;
        }

        artifact.image_size = manifest.preferred_artifact.image_size;
        artifact.wire_size = manifest.preferred_artifact.wire_size;
        artifact.encoding = manifest.preferred_artifact.zlib
                                ? OtaUploadEncoding::Zlib
                                : OtaUploadEncoding::Plain;
    }

    std::shared_ptr<const LargeByteBuffer> release_notes;
    if (update_available && manifest.release_notes_url[0]) {
        trace_phase("release_notes_fetch");
        char *resolved_url = reinterpret_cast<char *>(manifest_buffer);
        if (ota_resolve_release_artifact_url(
                url, manifest.release_notes_url, resolved_url,
                AC_OTA_URL_MAX_LENGTH + 1)) {
            std::unique_ptr<LargeByteBuffer> notes =
                LargeByteBuffer::allocate(AC_OTA_RELEASE_NOTES_MAX_BYTES);
            size_t notes_length = 0;
            OtaUrlError notes_error;
            if (notes && ota_url_fetch(
                    resolved_url, notes->data(), notes->size(), notes_length,
                    notes_error, continue_callback, &context, trace_callback) &&
                notes_length > 0 && notes->truncate(notes_length)) {
                release_notes = LargeByteBuffer::freeze(std::move(notes));
            } else if (!cancelled(generation)) {
                Log::logf(CAT_OTA, LOG_DEBUG,
                          "release notes unavailable error=%s http=%d\n",
                          notes ? notes_error.code
                                : "release_notes_alloc_failed",
                          notes_error.http_status);
            }
        } else {
            Log::logf(CAT_OTA, LOG_DEBUG,
                      "release notes unavailable error=invalid_url\n");
        }
    }

    if (cancelled(generation)) {
        Memory::free(manifest_buffer);
        finish_task(url, generation, nullptr, false, artifact, {}, "aborted",
                    false);
        return;
    }

    Memory::free(manifest_buffer);
    trace_phase("publish");
    finish_task(url, generation, &manifest, update_available, artifact,
                std::move(release_notes), nullptr, false);
}

void UpdateChecker::finish_task(
    char *url,
    uint32_t generation,
    const OtaReleaseManifest *manifest,
    bool update_available,
    OwnedArtifact artifact,
    std::shared_ptr<const LargeByteBuffer> release_notes,
    const char *error,
    bool retry_soon,
    const OtaUrlError *transport_error) {
    bool publish = false;
    const char *deferred_reason = nullptr;
    const uint32_t now = millis();

    if (lock()) {
        publish = generation == config_generation_;
        if (check_url_ == url) check_url_ = nullptr;
        if (publish && !cancel_requested_) {
            if (therapy_preempted_) deferred_reason = "therapy active";
            else if (export_preempted_) deferred_reason = "export pending";
        }

        if (deferred_reason) {
            status_.pending = true;
            status_.error = "";
        } else if (publish) {
            manual_requested_ = false;
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

                clear_release_locked();
                if (update_available && artifact.url) {
                    available_artifact_ = artifact;
                    artifact.url = nullptr;
                    status_.installable = true;
                }
                if (update_available && release_notes) {
                    release_notes_ = std::move(release_notes);
                    status_.release_notes_available = true;
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

    if (deferred_reason) {
        trace_phase(strcmp(deferred_reason, "therapy active") == 0
                        ? "deferred_therapy" : "deferred_export");
        Log::logf(CAT_OTA, LOG_DEBUG,
                  "firmware update check deferred: %s\n", deferred_reason);
        return;
    }

    trace_phase(error && *error ? "failed" : "finished");
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

void UpdateChecker::clear_release_locked() {
    if (available_artifact_.url) Memory::free(available_artifact_.url);
    available_artifact_ = {};
    release_notes_.reset();
    status_.installable = false;
    status_.release_notes_available = false;
}

bool UpdateChecker::cancelled(uint32_t generation) {
    if (!lock()) return true;

    if (!manual_requested_ && exports_->endpoint_work_claimed()) {
        export_preempted_ = true;
    }
    const bool result = cancel_requested_ || export_preempted_ ||
                        therapy_preempted_ ||
                        generation != config_generation_;
    unlock();
    return result;
}

bool UpdateChecker::continue_callback(void *ctx) {
    UpdateCheckContext *context = static_cast<UpdateCheckContext *>(ctx);
    return context && context->checker &&
           !context->checker->cancelled(context->generation);
}

void UpdateChecker::trace_callback(void *ctx, const char *stage) {
    auto *context = static_cast<UpdateCheckContext *>(ctx);
    context->checker->trace_phase(stage);
}

void UpdateChecker::begin_trace(const char *source) {
    if (++trace_id_ == 0) ++trace_id_;
    trace_started_ms_ = trace_phase_ms_ = trace_logged_ms_ = millis();
    trace_phase_ = "queued";
    Log::logf(CAT_OTA, LOG_DEBUG, "update check id=%lu queued source=%s\n",
              static_cast<unsigned long>(trace_id_), source);
}

void UpdateChecker::trace_phase(const char *phase) {
    if (Log::get_cat_level(CAT_OTA) < LOG_DEBUG) return;

    if (!lock()) return;
    const uint32_t now = millis();
    const bool changed = strcmp(trace_phase_, phase) != 0;
    if (changed || static_cast<uint32_t>(now - trace_logged_ms_) >= 30000) {
        Log::logf(CAT_OTA, LOG_DEBUG,
                  "update check id=%lu stage=%s previous=%s stage_ms=%lu elapsed_ms=%lu\n",
                  static_cast<unsigned long>(trace_id_), phase, trace_phase_,
                  static_cast<unsigned long>(now - trace_phase_ms_),
                  static_cast<unsigned long>(now - trace_started_ms_));
        if (changed) trace_phase_ms_ = now;
        trace_logged_ms_ = now;
        trace_phase_ = phase;
    }
    unlock();
}

bool UpdateChecker::lock(TickType_t timeout) const {
    return !mutex_ || xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE;
}

void UpdateChecker::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
