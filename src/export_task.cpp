#include "export_task.h"

#include <new>
#include <stdio.h>

#include "board_net.h"
#include "board_report.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_export_plan.h"

namespace aircannect {

struct ExportTask::Runtime {
    StorageSyncEngine smb;
    SleepHqSyncEngine sleephq;
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
    inputs_.config = config;
    runtime_->smb.begin(config.smb, scan_port, read_port, stream_port,
                        write_port, path_port);
    runtime_->sleephq.begin(config.sleephq, scan_port, read_port, stream_port,
                            write_port, path_port);
    applied_config_generation_ = inputs_.config_generation;

    status_.initialized = true;
    publish_status();

    const BaseType_t created = xTaskCreatePinnedToCore(
        task_entry, "ac_export", AC_EXPORT_TASK_STACK, this,
        AC_EXPORT_TASK_PRIO, &task_, AC_EXPORT_TASK_CORE);
    if (created != pdPASS) {
        runtime_->~Runtime();
        Memory::free(runtime_);
        runtime_ = nullptr;
        task_ = nullptr;
        Log::logf(CAT_EXPORT, LOG_ERROR,
                  "export task creation failed\n");
        return false;
    }

    Log::logf(CAT_EXPORT, LOG_INFO,
              "export task started core=%u prio=%u stack=%u\n",
              static_cast<unsigned>(AC_EXPORT_TASK_CORE),
              static_cast<unsigned>(AC_EXPORT_TASK_PRIO),
              static_cast<unsigned>(AC_EXPORT_TASK_STACK));
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
    if (!lock_inputs()) return;

    const bool changed =
        !export_endpoint_config_equal(inputs_.config, config);
    if (changed) {
        inputs_.config = config;
        inputs_.config_generation++;
        if (inputs_.config_generation == 0) inputs_.config_generation++;
    }

    unlock_inputs();
    if (changed) wake();
}

void ExportTask::publish_activity(const ActivitySnapshot &activity) {
    if (!lock_inputs()) return;

    const bool changed =
        inputs_.activity_generation != activity.generation;
    if (changed) {
        inputs_.activity = activity;
        inputs_.activity_generation = activity.generation;
    }

    unlock_inputs();
    if (changed) wake();
}

void ExportTask::publish_network(const NetworkSnapshot &network) {
    if (!lock_inputs()) return;

    const bool changed = inputs_.network_generation != network.generation;
    if (changed) {
        inputs_.network = network;
        inputs_.network_generation = network.generation;
    }

    unlock_inputs();
    if (changed) wake();
}

void ExportTask::defer_smb_until(uint32_t until_ms) {
    if (!lock_inputs()) return;

    inputs_.smb_defer_until_ms = until_ms;
    inputs_.smb_defer_generation++;
    if (inputs_.smb_defer_generation == 0) {
        inputs_.smb_defer_generation++;
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

    if (inputs_.command.kind != CommandKind::None) {
        unlock_inputs();

        if (status_lock_ &&
            xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
            status_.command_drops++;
            xSemaphoreGive(status_lock_);
        }
        return false;
    }

    inputs_.command.kind = kind;
    inputs_.command.sequence = next_command_sequence_++;
    if (next_command_sequence_ == 0) next_command_sequence_++;
    inputs_.command.day[0] = '\0';
    if (day) {
        snprintf(inputs_.command.day, sizeof(inputs_.command.day), "%s", day);
    }

    unlock_inputs();
    wake();
    return true;
}

bool ExportTask::request_smb_sync() {
    return queue_command(CommandKind::SmbSync);
}

bool ExportTask::request_smb_verify() {
    return queue_command(CommandKind::SmbVerify);
}

bool ExportTask::request_smb_post_therapy() {
    return queue_command(CommandKind::SmbPostTherapy);
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
    if (!lock_inputs()) return false;

    out = inputs_;
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
        case CommandKind::SmbVerify:
            configured = smb.enabled && smb.configured;
            accepted = runtime_->smb.request_verify_recent();
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
    if (!lock_inputs()) return;

    if (inputs_.command.sequence == command.sequence) {
        inputs_.command = Command();
    }

    unlock_inputs();
    if (failed && status_lock_ &&
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        status_.command_failures++;
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
    if (lock_inputs()) {
        command_pending = inputs_.command.kind != CommandKind::None;
        unlock_inputs();
    }
    if (!status_lock_ ||
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    status_.initialized = true;
    status_.task_started = task_ != nullptr;
    status_.network_ready = network_ready_;
    status_.runtime_blocked = runtime_blocked_;
    status_.command_pending = command_pending;
    status_.smb = runtime_->smb.status();
    status_.smb_runtime = runtime_->smb.runtime_status();
    status_.sleephq = runtime_->sleephq.status();
    status_.sleephq_runtime = runtime_->sleephq.runtime_status();
    status_.active = status_.smb_runtime.state == StorageSyncState::Working ||
                     status_.sleephq_runtime.state ==
                         SleepHqSyncState::Working;
    status_.busy = status_.active || status_.smb_runtime.pending ||
                   status_.sleephq_runtime.pending ||
                   status_.command_pending;
#if AC_STACK_PROFILE_ENABLED
    status_.stack_high_water_words = uxTaskGetStackHighWaterMark(nullptr);
#endif

    xSemaphoreGive(status_lock_);
}

ExportTaskStatus ExportTask::status() const {
    ExportTaskStatus out;
    if (status_lock_ &&
        xSemaphoreTake(status_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        out = status_;
        xSemaphoreGive(status_lock_);
    }

    if (lock_inputs()) {
        out.command_pending = inputs_.command.kind != CommandKind::None;
        out.busy = out.busy || out.command_pending;
        unlock_inputs();
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
        PublishedInputs inputs;
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
        const ExportTaskStatus current = status();
        if (result == ExportStep::Working) {
            delay_ms = AC_EXPORT_TASK_WORK_TICK_MS;
        } else if (result == ExportStep::Waiting || current.busy ||
                   (inputs.network.ipv4_ready && !network_ready_)) {
            delay_ms = AC_EXPORT_TASK_BUSY_RECHECK_MS;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

}  // namespace aircannect
