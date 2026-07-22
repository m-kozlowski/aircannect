#include "export_task.h"

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

void ExportTask::publish_config(const ExportEndpointConfig &config) {
    if (!runtime_) return;
    if (!lock_inputs()) return;

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
}

void ExportTask::publish_activity(const ActivitySnapshot &activity) {
    const bool blocked = runtime_blocked(activity);
    if (blocked && runtime_) {
        // The export task may be inside blocking network I/O. Its operation
        // controls must observe the gate before the task can process inputs.
        runtime_->smb.set_runtime_blocked(true);
        runtime_->sleephq.set_runtime_blocked(true);
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

void ExportTask::defer_smb_until(uint32_t until_ms) {
    if (!runtime_) return;
    if (!lock_inputs()) return;

    runtime_->inputs.smb_defer_until_ms = until_ms;
    runtime_->inputs.smb_defer_generation++;
    if (runtime_->inputs.smb_defer_generation == 0) {
        runtime_->inputs.smb_defer_generation++;
    }

    unlock_inputs();
    wake();
}

bool ExportTask::queue_command(CommandKind kind, const char *day) {
    if (!runtime_ || kind == CommandKind::None) return false;
    if (kind == CommandKind::SleepHqSyncDay &&
        (!day || !storage_export_is_datalog_day_name(day))) {
        return false;
    }
    if (!lock_inputs()) return false;

    if (runtime_->inputs.command.kind != CommandKind::None) {
        const bool same_day = kind != CommandKind::SleepHqSyncDay ||
                              strcmp(runtime_->inputs.command.day, day) == 0;
        const bool already_queued =
            runtime_->inputs.command.kind == kind && same_day;
        unlock_inputs();

        if (already_queued) return true;

        if (status_lock_ &&
            xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
            runtime_->control_status.command_drops++;
            xSemaphoreGive(status_lock_);
        }
        return false;
    }

    runtime_->inputs.command.kind = kind;
    runtime_->inputs.command.sequence = next_command_sequence_++;
    if (next_command_sequence_ == 0) next_command_sequence_++;
    runtime_->inputs.command.day[0] = '\0';
    if (day) {
        snprintf(runtime_->inputs.command.day,
                 sizeof(runtime_->inputs.command.day), "%s", day);
    }

    unlock_inputs();
    if (kind != CommandKind::SmbScheduledReconcile) {
        runtime_->smb.cancel_scheduled_reconcile();
    }
    wake();
    return true;
}

bool ExportTask::request_smb_sync() {
    return queue_command(CommandKind::SmbSync);
}

bool ExportTask::request_smb_startup_check() {
    return queue_command(CommandKind::SmbStartupCheck);
}

bool ExportTask::request_smb_verify() {
    return queue_command(CommandKind::SmbVerify);
}

bool ExportTask::request_smb_scheduled_reconcile() {
    return queue_command(CommandKind::SmbScheduledReconcile);
}

bool ExportTask::request_smb_post_therapy() {
    return queue_command(CommandKind::SmbPostTherapy);
}

void ExportTask::cancel_smb_scheduled_reconcile() {
    if (!runtime_) return;

    runtime_->smb.cancel_scheduled_reconcile();
    wake();
}

bool ExportTask::request_sleephq_check() {
    return queue_command(CommandKind::SleepHqCheck);
}

bool ExportTask::request_sleephq_startup_check() {
    return queue_command(CommandKind::SleepHqStartupCheck);
}

bool ExportTask::request_sleephq_sync() {
    return queue_command(CommandKind::SleepHqSync);
}

bool ExportTask::request_sleephq_idle_backfill() {
    return queue_command(CommandKind::SleepHqIdleBackfill);
}

bool ExportTask::request_sleephq_sync_day(const char *day) {
    return queue_command(CommandKind::SleepHqSyncDay, day);
}

bool ExportTask::request_sleephq_post_therapy() {
    return queue_command(CommandKind::SleepHqPostTherapy);
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

    if (inputs.smb_defer_generation != applied_smb_defer_generation_) {
        runtime_->smb.defer_idle_work_until(inputs.smb_defer_until_ms);
        applied_smb_defer_generation_ = inputs.smb_defer_generation;
    }

    runtime_->smb.set_network_available(network_ready_);
    runtime_->sleephq.set_network_available(network_ready_);
    runtime_->smb.set_runtime_blocked(runtime_blocked_);
    runtime_->sleephq.set_runtime_blocked(runtime_blocked_);
}

bool ExportTask::apply_command(const Command &command) {
    if (command.kind == CommandKind::None) return true;

    const StorageSyncRuntimeStatus smb = runtime_->smb.runtime_status();
    const SleepHqSyncRuntimeStatus sleephq = runtime_->sleephq.runtime_status();
    if (smb.state == StorageSyncState::Working || smb.pending ||
        sleephq.state == SleepHqSyncState::Working || sleephq.pending) {
        return false;
    }

    bool accepted = false;
    bool configured = true;
    switch (command.kind) {
        case CommandKind::SmbSync:
            configured = smb.enabled && smb.configured;
            accepted = runtime_->smb.request_manual_sync();
            break;
        case CommandKind::SmbStartupCheck:
            configured = smb.enabled && smb.configured;
            accepted = runtime_->smb.request_startup_check();
            break;
        case CommandKind::SmbVerify:
            configured = smb.enabled && smb.configured;
            accepted = runtime_->smb.request_manual_reconcile();
            break;
        case CommandKind::SmbScheduledReconcile:
            configured = smb.enabled && smb.configured;
            accepted = runtime_->smb.request_scheduled_reconcile();
            break;
        case CommandKind::SmbPostTherapy:
            configured = smb.enabled && smb.configured;
            accepted = runtime_->smb.request_post_therapy_sync();
            break;
        case CommandKind::SleepHqCheck:
            configured = sleephq.configured;
            accepted = runtime_->sleephq.request_check("manual");
            break;
        case CommandKind::SleepHqStartupCheck:
            configured = sleephq.configured;
            accepted = runtime_->sleephq.request_check("startup_check");
            break;
        case CommandKind::SleepHqSync:
            configured = sleephq.configured;
            accepted = runtime_->sleephq.request_sync("manual");
            break;
        case CommandKind::SleepHqIdleBackfill:
            configured = sleephq.configured;
            accepted = runtime_->sleephq.request_sync("idle_backfill");
            break;
        case CommandKind::SleepHqSyncDay:
            configured = sleephq.configured;
            accepted = runtime_->sleephq.request_sync_day(command.day,
                                                           "manual_day");
            break;
        case CommandKind::SleepHqPostTherapy:
            configured = sleephq.configured;
            accepted = runtime_->sleephq.request_post_therapy_sync();
            break;
        case CommandKind::None:
            return true;
    }

    if (!configured) {
        finish_command(command, true);
        return true;
    }
    if (!accepted) return false;

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
    const StorageSyncRuntimeStatus smb = runtime_->smb.runtime_status();
    const SleepHqSyncRuntimeStatus sleephq = runtime_->sleephq.runtime_status();

    if (smb.state == StorageSyncState::Working) return runtime_->smb.step();
    if (sleephq.state == SleepHqSyncState::Working) {
        return runtime_->sleephq.step();
    }

    ExportStep result = ExportStep::Idle;
    if (next_idle_endpoint_smb_) {
        result = runtime_->smb.step();
    } else {
        result = runtime_->sleephq.step();
    }
    next_idle_endpoint_smb_ = !next_idle_endpoint_smb_;
    return result;
}

void ExportTask::publish_status() {
    if (!runtime_) return;

    bool command_pending = false;
    char smb_endpoint[AC_SMB_EXPORT_ENDPOINT_MAX] = {};
    char sleephq_team_id[AC_SLEEPHQ_ID_MAX] = {};
    char sleephq_device_id[AC_SLEEPHQ_ID_MAX] = {};
    if (lock_inputs()) {
        command_pending =
            runtime_->inputs.command.kind != CommandKind::None;
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
    control.smb = runtime_->smb.runtime_status();
    sleephq.sync = runtime_->sleephq.status();
    control.sleephq = runtime_->sleephq.runtime_status();
    control.active = control.smb.state == StorageSyncState::Working ||
                     control.sleephq.state == SleepHqSyncState::Working;
    control.busy = control.active || control.smb.pending ||
                   control.sleephq.pending || control.command_pending;
#if AC_STACK_PROFILE_ENABLED
    control.stack_high_water_words = uxTaskGetStackHighWaterMark(nullptr);
#endif

    xSemaphoreGive(status_lock_);
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
            runtime_->inputs.command.kind != CommandKind::None;
        out.busy = out.busy || out.command_pending;
        unlock_inputs();
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
        (void)apply_command(inputs.command);

        const ExportStep result = step_endpoint();
        publish_status();

        uint32_t delay_ms = AC_EXPORT_TASK_IDLE_TICK_MS;
        const StorageSyncRuntimeStatus smb = runtime_->smb.runtime_status();

        const SleepHqSyncRuntimeStatus sleephq =
            runtime_->sleephq.runtime_status();

        const bool busy = smb.state == StorageSyncState::Working ||
                          smb.pending ||
                          sleephq.state == SleepHqSyncState::Working ||
                          sleephq.pending ||
                          inputs.command.kind != CommandKind::None;

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
