#include "export_task.h"
#include "export_backends.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>

#include "board_net.h"
#include "board_report.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_export_plan.h"
#include "string_util.h"

namespace aircannect {

struct ExportTask::Runtime {
    StorageSyncEngine smb;
    SleepHqSyncEngine sleephq;
    ExportBackend installed_backends[2] = {
        make_export_backend(smb), make_export_backend(sleephq),
    };
    ExportBackendRegistry backends{installed_backends, std::size(installed_backends)};
    PublishedInputs inputs;
    PublishedInputs input_snapshot;
    ExportTaskControlSnapshot control_status;
    ExportSmbStatusSnapshot smb_status;
    ExportSleepHqStatusSnapshot sleephq_status;
    bool task_stack_external = false;
};

bool ExportTask::begin(const ExportEndpointConfig &config,
                       StorageScanPort &scan_port,
                       StorageReadPort &read_port,
                       StorageStreamPort &stream_port,
                       StorageAtomicWritePort &write_port,
                       StoragePathPort &path_port) {
    if (runtime_) return true;

    input_lock_ = xSemaphoreCreateMutexStatic(&input_lock_storage_);
    status_lock_ = xSemaphoreCreateMutexStatic(&status_lock_storage_);
    if (!input_lock_ || !status_lock_) return false;

    void *memory = Memory::alloc_large(sizeof(Runtime), false);
    if (!memory) {
        Log::logf(CAT_EXPORT, LOG_ERROR,
                  "export task runtime allocation failed\n");
        return false;
    }

    runtime_ = new (memory) Runtime();
    runtime_->inputs.config = config;
    runtime_->smb.begin(config.smb, scan_port, read_port, stream_port,
                        write_port, path_port);
    runtime_->sleephq.begin(config.sleephq, scan_port, read_port, stream_port,
                            write_port, path_port);
    applied_config_generation_ = runtime_->inputs.config_generation;

    runtime_->control_status.initialized = true;
    publish_status();

    BaseType_t created = pdFAIL;
    if (Memory::psram_available()) {
        created = xTaskCreatePinnedToCoreWithCaps(
            task_entry, "ac_export", AC_EXPORT_TASK_STACK, this,
            AC_EXPORT_TASK_PRIO, &task_, AC_EXPORT_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        runtime_->task_stack_external =
            created == pdPASS && task_ != nullptr;
    }

    if (!runtime_->task_stack_external) {
        task_ = nullptr;
        created = xTaskCreatePinnedToCore(
            task_entry, "ac_export", AC_EXPORT_TASK_STACK, this,
            AC_EXPORT_TASK_PRIO, &task_, AC_EXPORT_TASK_CORE);
    }

    if (created != pdPASS || !task_) {
        runtime_->~Runtime();
        Memory::free(runtime_);
        runtime_ = nullptr;
        task_ = nullptr;
        Log::logf(CAT_EXPORT, LOG_ERROR,
                  "export task creation failed\n");
        return false;
    }

    Log::logf(CAT_EXPORT, LOG_INFO,
              "export task started core=%u prio=%u stack=%u memory=%s\n",
              static_cast<unsigned>(AC_EXPORT_TASK_CORE),
              static_cast<unsigned>(AC_EXPORT_TASK_PRIO),
              static_cast<unsigned>(AC_EXPORT_TASK_STACK),
              runtime_->task_stack_external ? "psram" : "internal");
    return true;
}

bool ExportTask::lock_inputs(uint32_t timeout_ms) const {
    return input_lock_ &&
           xSemaphoreTake(input_lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ExportTask::unlock_inputs() const {
    if (input_lock_) xSemaphoreGive(input_lock_);
}

bool ExportTask::publish_config(const ExportEndpointConfig &config) {
    if (!runtime_ || !lock_inputs()) return false;

    const bool changed =
        !export_endpoint_config_equal(runtime_->inputs.config, config);
    if (changed) {
        runtime_->inputs.config = config;
        runtime_->inputs.config_generation++;
        if (runtime_->inputs.config_generation == 0) {
            runtime_->inputs.config_generation++;
        }
    }

    unlock_inputs();
    if (changed) wake();
    return true;
}

void ExportTask::publish_activity(const ActivitySnapshot &activity) {
    const bool blocked = runtime_blocked(activity);
    if (blocked && runtime_) {
        // The export task may be inside blocking network I/O. Its operation
        // controls must observe the gate before the task can process inputs.
        for (const auto &backend : runtime_->backends) {
            backend.set_runtime_blocked(true);
        }
    }

    if (!runtime_ || !lock_inputs()) return;

    const bool changed =
        runtime_->inputs.activity_generation != activity.generation;
    if (changed) {
        runtime_->inputs.activity = activity;
        runtime_->inputs.activity_generation = activity.generation;
    }

    unlock_inputs();
    if (changed) wake();
}

void ExportTask::publish_network(const NetworkSnapshot &network) {
    if (!runtime_) return;
    if (!lock_inputs()) return;

    const bool changed =
        runtime_->inputs.network_generation != network.generation;
    if (changed) {
        runtime_->inputs.network = network;
        runtime_->inputs.network_generation = network.generation;
    }

    unlock_inputs();
    if (changed) wake();
}

void ExportTask::defer_idle_until(ExportEndpoint endpoint, uint32_t until_ms) {
    if (!runtime_) return;
    if (!runtime_->backends.find(endpoint)) return;
    if (!lock_inputs()) return;

    runtime_->inputs.defer_until_ms[static_cast<size_t>(endpoint)] = until_ms;
    runtime_->inputs.defer_generation++;
    if (runtime_->inputs.defer_generation == 0) {
        runtime_->inputs.defer_generation++;
    }

    unlock_inputs();
    wake();
}

bool ExportTask::request(const ExportRequest &request) {
    if (storage_suspend_requested_.load() || !runtime_) return false;
    const auto *backend = runtime_->backends.find(request.endpoint);
    if (!backend || !request.supported(backend->definition())) return false;
    if (!lock_inputs()) return false;

    if (storage_suspend_requested_.load()) {
        unlock_inputs();
        return false;
    }

    Command &command = runtime_->inputs.command;
    if (command.sequence != 0) {
        const ExportRequest &queued = command.request;
        const bool already_queued =
            queued.endpoint == request.endpoint &&
            queued.operation == request.operation &&
            queued.reason == request.reason &&
            strcmp(queued.day, request.day) == 0;
        unlock_inputs();

        if (already_queued) return true;

        if (status_lock_ &&
            xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
            runtime_->control_status.command_drops++;
            xSemaphoreGive(status_lock_);
        }
        return false;
    }

    command.request = request;
    command.sequence = next_command_sequence_++;
    if (next_command_sequence_ == 0) next_command_sequence_++;

    work_reservation_.fetch_or(ExportDemand);
    unlock_inputs();
    const auto incoming = backend->definition().rule(
        request.operation, request.reason)->work;
    for (const auto &other : runtime_->backends) {
        const auto status = other.status();
        if (status.maintenance_pending && status.can_preempt(incoming)) {
            other.cancel(ExportReason::Maintenance);
        }
    }
    wake();
    return true;
}

void ExportTask::cancel_maintenance(ExportEndpoint endpoint) {
    if (!runtime_) return;

    const auto *backend = runtime_->backends.find(endpoint);
    if (backend) backend->cancel(ExportReason::Maintenance);
    wake();
}

bool ExportTask::cancel_post_therapy() {
    if (!runtime_ || !lock_inputs(0)) return false;

    if (runtime_->inputs.command.sequence != 0 &&
        runtime_->inputs.command.request.reason == ExportReason::PostTherapy) {
        runtime_->inputs.command = Command();
    }
    unlock_inputs();

    for (const auto &backend : runtime_->backends) {
        backend.cancel(ExportReason::PostTherapy);
    }
    wake();
    return true;
}

bool ExportTask::copy_inputs(PublishedInputs &out) const {
    if (!runtime_) return false;
    if (!lock_inputs()) return false;

    out = runtime_->inputs;
    unlock_inputs();
    return true;
}

bool ExportTask::runtime_blocked(const ActivitySnapshot &activity) const {
    return activity.therapy_active || activity.realtime_stream_active ||
           activity.foreground_report_demand || activity.ota_install_active;
}

void ExportTask::update_network_ready(const NetworkSnapshot &network,
                                      uint32_t now_ms) {
    if (!network.ipv4_ready) {
        network_ready_since_ms_ = 0;
        network_ready_ = false;
        return;
    }

    if (network_ready_since_ms_ == 0) {
        network_ready_since_ms_ = now_ms == 0 ? 1 : now_ms;
        network_ready_ = false;
        return;
    }

    network_ready_ =
        static_cast<int32_t>(now_ms - network_ready_since_ms_) >=
        static_cast<int32_t>(AC_EXPORT_NETWORK_SETTLE_MS);
}

void ExportTask::apply_inputs(const PublishedInputs &inputs,
                              uint32_t now_ms) {
    if (inputs.config_generation != applied_config_generation_) {
        runtime_->smb.configure(inputs.config.smb);
        runtime_->sleephq.configure(inputs.config.sleephq);
        applied_config_generation_ = inputs.config_generation;
    }

    if (inputs.network_generation != applied_network_generation_) {
        network_ready_since_ms_ = 0;
        network_ready_ = false;
        applied_network_generation_ = inputs.network_generation;
    }
    update_network_ready(inputs.network, now_ms);

    if (inputs.activity_generation != applied_activity_generation_) {
        runtime_blocked_ = runtime_blocked(inputs.activity);
        applied_activity_generation_ = inputs.activity_generation;
    }

    if (inputs.defer_generation != applied_defer_generation_) {
        for (const auto &backend : runtime_->backends) {
            backend.defer_idle_work_until(
                inputs.defer_until_ms[static_cast<size_t>(backend.definition().id)]);
        }
        applied_defer_generation_ = inputs.defer_generation;
    }

    for (const auto &backend : runtime_->backends) {
        backend.set_network_available(network_ready_);
    }
}

bool ExportTask::apply_command(const Command &command) {
    if (command.sequence == 0) return true;

    for (const auto &backend : runtime_->backends) {
        if (backend.status().busy()) return false;
    }

    const auto *backend = runtime_->backends.find(command.request.endpoint);
    if (!backend || !backend->status().configured) {
        finish_command(command, true);
        return true;
    }
    if (!backend->request(command.request)) return false;

    finish_command(command, false);
    return true;
}

void ExportTask::finish_command(const Command &command, bool failed) {
    if (!runtime_ || !input_lock_ ||
        xSemaphoreTake(input_lock_, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (runtime_->inputs.command.sequence == command.sequence) {
        runtime_->inputs.command = Command();
    }

    xSemaphoreGive(input_lock_);
    if (failed && status_lock_ &&
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        runtime_->control_status.command_failures++;
        xSemaphoreGive(status_lock_);
    }
}

ExportStep ExportTask::step_endpoint() {
    // Local completion must drain before another endpoint can use storage.
    for (const auto &backend : runtime_->backends) {
        if (backend.status().durable_state_pending) return backend.step();
    }
    for (const auto &backend : runtime_->backends) {
        if (backend.status().executing()) return backend.step();
    }

    const ExportStep result = runtime_->backends[next_idle_endpoint_].step();
    next_idle_endpoint_ = (next_idle_endpoint_ + 1) % runtime_->backends.count;
    return result;
}

bool ExportTask::publish_work_claim() {
    if (!runtime_ || !lock_inputs(0)) return false;

    bool active = false;
    bool pending = runtime_->inputs.command.sequence != 0;
    for (const auto &backend : runtime_->backends) {
        const auto status = backend.status();
        active = active || status.executing();
        pending = pending || status.awaiting();
    }
    const bool ready_pending = pending && network_ready_ && !runtime_blocked_;

    uint8_t current = work_reservation_.load();
    uint8_t next;
    do {
        next = static_cast<uint8_t>(current & ~(ExportDemand | EndpointActive));
        if (active || ready_pending) next |= ExportDemand;
        if (active) next |= EndpointActive;
    } while (!work_reservation_.compare_exchange_weak(current, next));
    unlock_inputs();
    return true;
}

void ExportTask::publish_status() {
    if (!runtime_) return;

    bool command_pending = false;
    char smb_endpoint[AC_SMB_EXPORT_ENDPOINT_MAX] = {};
    char sleephq_team_id[AC_SLEEPHQ_ID_MAX] = {};
    char sleephq_device_id[AC_SLEEPHQ_ID_MAX] = {};
    if (lock_inputs()) {
        command_pending =
            runtime_->inputs.command.sequence != 0;
        copy_cstr(smb_endpoint, sizeof(smb_endpoint),
                  runtime_->inputs.config.smb.endpoint);
        copy_cstr(sleephq_team_id, sizeof(sleephq_team_id),
                  runtime_->inputs.config.sleephq.team_id);
        copy_cstr(sleephq_device_id, sizeof(sleephq_device_id),
                  runtime_->inputs.config.sleephq.device_id);
        unlock_inputs();
    }
    if (!status_lock_ ||
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    ExportTaskControlSnapshot &control = runtime_->control_status;
    ExportSmbStatusSnapshot &smb = runtime_->smb_status;
    ExportSleepHqStatusSnapshot &sleephq = runtime_->sleephq_status;

    control.initialized = true;
    control.task_started = task_ != nullptr;
    control.network_ready = network_ready_;
    control.runtime_blocked = runtime_blocked_;
    control.command_pending = command_pending;
    copy_cstr(smb.smb_endpoint, sizeof(smb.smb_endpoint),
              smb_endpoint);
    copy_cstr(sleephq.sleephq_team_id,
              sizeof(sleephq.sleephq_team_id),
              sleephq_team_id);
    copy_cstr(sleephq.sleephq_device_id,
              sizeof(sleephq.sleephq_device_id),
              sleephq_device_id);
    smb.sync = runtime_->smb.status();
    sleephq.sync = runtime_->sleephq.status();

    control.active = false;
    control.busy = command_pending;
    control.order.count = 0;
    for (const auto &backend : runtime_->backends) {
        const auto status = backend.status();
        const auto id = backend.definition().id;
        control.order.ids[control.order.count++] = id;
        control.endpoint(id) = status;
        control.active = control.active || status.executing();
        control.busy = control.busy || status.busy();
    }

    xSemaphoreGive(status_lock_);
}

bool ExportTask::endpoint_work_claimed() const {
    return (work_reservation_.load() & ExportDemand) != 0;
}

bool ExportTask::try_begin_update_check(bool manual) {
    uint8_t current = work_reservation_.load();
    const uint8_t blocked = EndpointStep | EndpointActive | UpdateCheck |
                            (manual ? 0 : ExportDemand);
    while ((current & blocked) == 0) {
        if (work_reservation_.compare_exchange_weak(
                current, static_cast<uint8_t>(current | UpdateCheck))) {
            return true;
        }
    }
    return false;
}

void ExportTask::end_update_check() {
    work_reservation_.fetch_and(static_cast<uint8_t>(~UpdateCheck));
    wake();
}

bool ExportTask::try_begin_endpoint_step() {
    uint8_t current = work_reservation_.load();
    while ((current & UpdateCheck) == 0) {
        if (work_reservation_.compare_exchange_weak(
                current, static_cast<uint8_t>(current | EndpointStep))) {
            return true;
        }
    }
    return false;
}

ExportTaskControlSnapshot ExportTask::control_snapshot() const {
    ExportTaskControlSnapshot out;
    if (runtime_ && status_lock_ &&
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        out = runtime_->control_status;
        xSemaphoreGive(status_lock_);
    }

    if (runtime_ && lock_inputs()) {
        out.command_pending =
            runtime_->inputs.command.sequence != 0;
        out.busy = out.busy || out.command_pending;
        unlock_inputs();
    }
    return out;
}

ExportStatusSnapshot ExportTask::status_snapshot() const {
    ExportStatusSnapshot out;
    if (runtime_ && status_lock_ &&
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        out.smb = runtime_->smb_status;
        out.sleephq = runtime_->sleephq_status;
        xSemaphoreGive(status_lock_);
    }
    return out;
}

ExportSmbStatusSnapshot ExportTask::smb_status() const {
    ExportSmbStatusSnapshot out;
    if (runtime_ && status_lock_ &&
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        out = runtime_->smb_status;
        xSemaphoreGive(status_lock_);
    }
    return out;
}

ExportSleepHqStatusSnapshot ExportTask::sleephq_status() const {
    ExportSleepHqStatusSnapshot out;
    if (runtime_ && status_lock_ &&
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        out = runtime_->sleephq_status;
        xSemaphoreGive(status_lock_);
    }
    return out;
}

#if AC_STACK_PROFILE_ENABLED
uint32_t ExportTask::stack_high_water_bytes() const {
    return task_ ? uxTaskGetStackHighWaterMark(task_) : 0;
}
#endif

void ExportTask::wake() {
    if (task_) xTaskNotifyGive(task_);
}

void ExportTask::task_entry(void *context) {
    static_cast<ExportTask *>(context)->run();
}

void ExportTask::set_storage_suspended(bool suspended) {
    if (storage_suspend_requested_.exchange(suspended) == suspended) return;
    if (suspended && runtime_) {
        for (const auto &backend : runtime_->backends) {
            backend.set_runtime_blocked(true);
        }
    }
    wake();
}

void ExportTask::run() {
    for (;;) {
        PublishedInputs &inputs = runtime_->input_snapshot;
        if (!copy_inputs(inputs)) {
            ulTaskNotifyTake(pdTRUE,
                             pdMS_TO_TICKS(AC_EXPORT_TASK_BUSY_RECHECK_MS));
            continue;
        }

        const uint32_t now_ms = millis();
        apply_inputs(inputs, now_ms);
        if (storage_suspend_requested_.load()) {
            if (!storage_paused_.load()) {
                bool released = true;
                for (const auto &backend : runtime_->backends) {
                    released = backend.release_media() && released;
                }
                if (released) {
                    finish_command(inputs.command, false);
                    if (publish_work_claim()) {
                        work_reservation_.fetch_and(static_cast<uint8_t>(~EndpointStep));
                        publish_status();
                        storage_paused_.store(true);
                    }
                }
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        storage_paused_.store(false);
        (void)apply_command(inputs.command);

        // Block run startup, but still let engines publish due retries and
        // process cancellation/configuration while a check is shutting down.
        const bool admitted = try_begin_endpoint_step();
        for (const auto &backend : runtime_->backends) {
            backend.set_runtime_blocked(runtime_blocked_ || !admitted);
        }

        const ExportStep result = step_endpoint();
        const bool claim_published = publish_work_claim();
        if (admitted && claim_published) {
            work_reservation_.fetch_and(static_cast<uint8_t>(~EndpointStep));
        }
        publish_status();

        uint32_t delay_ms = AC_EXPORT_TASK_IDLE_TICK_MS;
        bool busy = inputs.command.sequence != 0;
        for (const auto &backend : runtime_->backends) {
            busy = busy || backend.status().busy();
        }

        if (result == ExportStep::Working) {
            delay_ms = AC_EXPORT_TASK_WORK_TICK_MS;
        } else if (result == ExportStep::Waiting || busy ||
                   (inputs.network.ipv4_ready && !network_ready_)) {
            delay_ms = AC_EXPORT_TASK_BUSY_RECHECK_MS;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

}  // namespace aircannect
