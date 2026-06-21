#include "export_coordinator.h"

#include "board_report.h"
#include "debug_log.h"

namespace aircannect {

uint32_t ExportCoordinator::due_after(uint32_t now_ms, uint32_t delay_ms) {
    uint32_t due = now_ms + delay_ms;
    if (due == 0) due = 1;
    return due;
}

void ExportCoordinator::begin(StorageSyncJob *storage_sync,
                              SleepHqSyncJob *sleephq_sync) {
    storage_sync_ = storage_sync;
    sleephq_sync_ = sleephq_sync;
}

bool ExportCoordinator::request_smb_sync() {
    if (!storage_sync_ ||
        (sleephq_sync_ && sleephq_sync_->runtime_status().active())) {
        return false;
    }
    return storage_sync_->request_manual_sync();
}

bool ExportCoordinator::request_smb_verify() {
    if (!storage_sync_ ||
        (sleephq_sync_ && sleephq_sync_->runtime_status().active())) {
        return false;
    }
    return storage_sync_->request_verify_recent();
}

bool ExportCoordinator::request_sleephq_sync() {
    if (!sleephq_sync_ ||
        (storage_sync_ && storage_sync_->runtime_status().active())) {
        return false;
    }
    return sleephq_sync_->request_sync("manual");
}

bool ExportCoordinator::request_sleephq_check() {
    if (!sleephq_sync_ ||
        (storage_sync_ && storage_sync_->runtime_status().active())) {
        return false;
    }
    return sleephq_sync_->request_check("manual");
}

void ExportCoordinator::poll(RpcArbiter &arbiter,
                             ReportManager &report,
                             const AppConfigData &config,
                             bool network_connected,
                             bool resmed_ota_active,
                             bool esp_ota_active,
                             uint32_t now_ms) {
    if (storage_sync_) {
        storage_sync_->set_network_available(network_connected);
        storage_sync_->refresh_config(config, now_ms);
    }

    if (sleephq_sync_) {
        sleephq_sync_->set_network_available(network_connected);
        sleephq_sync_->refresh_config(config, now_ms);
    }

    const StorageSyncRuntimeStatus storage_runtime =
        storage_sync_ ? storage_sync_->runtime_status()
                      : StorageSyncRuntimeStatus();
    const SleepHqSyncRuntimeStatus sleephq_runtime =
        sleephq_sync_ ? sleephq_sync_->runtime_status()
                      : SleepHqSyncRuntimeStatus();
    const bool storage_sync_active = storage_runtime.active();
    const bool sleephq_working =
        sleephq_runtime.state == SleepHqSyncState::Working;

    poll_post_therapy(arbiter, report, storage_sync_active, now_ms);

    if (storage_sync_ && sleephq_working) {
        storage_sync_->defer_idle_work_until(
            now_ms + AC_BG_WORKER_BUSY_RECHECK_MS);
    }

    if (sleephq_sync_) {
        sleephq_sync_->set_runtime_blocked(
            report.foreground_busy() ||
            storage_sync_active ||
            arbiter.stream_activity_active() ||
            resmed_ota_active ||
            esp_ota_active ||
            arbiter.as11_state().therapy_state() ==
                As11TherapyState::Running);
    }

    maybe_queue_sleephq_startup_check(network_connected,
                                      storage_sync_active,
                                      sleephq_runtime);
    poll_sleephq_idle_backfill(arbiter,
                               report,
                               network_connected,
                               storage_sync_active,
                               sleephq_runtime,
                               now_ms);
}

void ExportCoordinator::poll_post_therapy(RpcArbiter &arbiter,
                                          ReportManager &report,
                                          bool storage_sync_active,
                                          uint32_t now_ms) {
    const As11TherapyState state = arbiter.as11_state().therapy_state();

    if (state == As11TherapyState::Running) {
        reset_post_therapy_after_running();
        post_therapy_.last_state = state;
        return;
    }

    if (post_therapy_.last_state == As11TherapyState::Running &&
        state != As11TherapyState::Running) {
        arm_post_therapy_after_stop(now_ms);
    }

    maybe_refresh_summary(arbiter, report, now_ms);
    maybe_queue_storage_sync(arbiter, report, now_ms);
    maybe_queue_post_therapy_sleephq(arbiter,
                                     report,
                                     storage_sync_active,
                                     now_ms);
    post_therapy_.last_state = state;
}

void ExportCoordinator::reset_post_therapy_after_running() {
    post_therapy_.summary_refresh_due_ms = 0;
    post_therapy_.storage_pending = false;
    post_therapy_.storage_grace_armed = false;
    post_therapy_.storage_due_ms = 0;
    post_therapy_.storage_deadline_ms = 0;
    post_therapy_.sleephq_pending = false;
    post_therapy_.sleephq_grace_armed = false;
    post_therapy_.sleephq_due_ms = 0;
    post_therapy_.sleephq_deadline_ms = 0;
}

void ExportCoordinator::arm_post_therapy_after_stop(uint32_t now_ms) {
    const StorageSyncRuntimeStatus storage_runtime =
        storage_sync_ ? storage_sync_->runtime_status()
                      : StorageSyncRuntimeStatus();
    const SleepHqSyncRuntimeStatus sleephq_runtime =
        sleephq_sync_ ? sleephq_sync_->runtime_status()
                      : SleepHqSyncRuntimeStatus();
    post_therapy_.summary_refresh_due_ms =
        due_after(now_ms, AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS);
    post_therapy_.storage_pending =
        storage_sync_ && storage_runtime.enabled &&
        storage_runtime.configured;
    post_therapy_.sleephq_pending =
        sleephq_sync_ && sleephq_runtime.configured;
    post_therapy_.storage_grace_armed = false;
    post_therapy_.sleephq_grace_armed = false;
    post_therapy_.storage_due_ms = 0;
    post_therapy_.sleephq_due_ms = 0;
    post_therapy_.storage_deadline_ms =
        due_after(now_ms, AC_REPORT_POST_THERAPY_SYNC_MAX_WAIT_MS);
    post_therapy_.sleephq_deadline_ms =
        due_after(now_ms, AC_SLEEPHQ_POST_THERAPY_SYNC_MAX_WAIT_MS);
    if (storage_sync_) {
        storage_sync_->defer_idle_work_until(
            post_therapy_.summary_refresh_due_ms);
    }
}

void ExportCoordinator::maybe_refresh_summary(RpcArbiter &arbiter,
                                              ReportManager &report,
                                              uint32_t now_ms) {
    if (post_therapy_.summary_refresh_due_ms == 0 ||
        static_cast<int32_t>(now_ms -
                             post_therapy_.summary_refresh_due_ms) < 0) {
        return;
    }
    if (arbiter.stream_activity_active()) {
        post_therapy_.summary_refresh_due_ms =
            due_after(now_ms, AC_BG_WORKER_BUSY_RECHECK_MS);
        if (storage_sync_) {
            storage_sync_->defer_idle_work_until(
                post_therapy_.summary_refresh_due_ms);
        }
        return;
    }
    if (report.request_summary_refresh()) {
        post_therapy_.summary_refresh_due_ms = 0;
        post_therapy_.storage_grace_armed = false;
        post_therapy_.storage_due_ms = 0;
    } else {
        post_therapy_.summary_refresh_due_ms =
            due_after(now_ms, AC_BG_WORKER_BUSY_RECHECK_MS);
    }
}

void ExportCoordinator::maybe_queue_storage_sync(RpcArbiter &arbiter,
                                                 ReportManager &report,
                                                 uint32_t now_ms) {
    if (!post_therapy_.storage_pending ||
        post_therapy_.summary_refresh_due_ms != 0) {
        return;
    }

    if (!storage_sync_) {
        post_therapy_.storage_pending = false;
        post_therapy_.storage_due_ms = 0;
        post_therapy_.storage_deadline_ms = 0;
        return;
    }
    if (arbiter.stream_activity_active()) {
        post_therapy_.storage_due_ms = 0;
        storage_sync_->defer_idle_work_until(
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS));
        return;
    }
    if (report.background_work_active()) {
        const bool deadline_reached =
            post_therapy_.storage_deadline_ms != 0 &&
            static_cast<int32_t>(now_ms -
                                 post_therapy_.storage_deadline_ms) >= 0;
        if (!deadline_reached) {
            post_therapy_.storage_due_ms = 0;
            storage_sync_->defer_idle_work_until(
                due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS));
            return;
        }
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] post-therapy sync fallback after report wait\n");
        queue_post_therapy_storage_sync(now_ms);
        return;
    }
    if (!post_therapy_.storage_grace_armed) {
        post_therapy_.storage_grace_armed = true;
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
        storage_sync_->defer_idle_work_until(post_therapy_.storage_due_ms);
        return;
    }
    if (post_therapy_.storage_due_ms == 0) {
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
        storage_sync_->defer_idle_work_until(post_therapy_.storage_due_ms);
        return;
    }
    if (static_cast<int32_t>(now_ms - post_therapy_.storage_due_ms) >= 0) {
        queue_post_therapy_storage_sync(now_ms);
    }
}

void ExportCoordinator::maybe_queue_post_therapy_sleephq(
    RpcArbiter &arbiter,
    ReportManager &report,
    bool storage_sync_active,
    uint32_t now_ms) {
    if (!post_therapy_.sleephq_pending) {
        return;
    }
    const bool deadline_reached =
        post_therapy_.sleephq_deadline_ms != 0 &&
        static_cast<int32_t>(now_ms -
                             post_therapy_.sleephq_deadline_ms) >= 0;
    if (post_therapy_.summary_refresh_due_ms != 0) {
        if (deadline_reached) {
            Log::logf(CAT_SLEEPHQ, LOG_WARN,
                      "post-therapy sync skipped after summary wait\n");
            clear_post_therapy_sleephq();
        }
        return;
    }
    if (!sleephq_sync_) {
        clear_post_therapy_sleephq();
        return;
    }
    const bool storage_blocking = post_therapy_.storage_pending ||
                                  storage_sync_active;
    if (arbiter.stream_activity_active() ||
        report.background_work_active() ||
        storage_blocking) {
        if (deadline_reached) {
            Log::logf(CAT_SLEEPHQ, LOG_WARN,
                      "post-therapy sync skipped after endpoint wait\n");
            clear_post_therapy_sleephq();
            return;
        }
        post_therapy_.sleephq_due_ms = 0;
        return;
    }
    if (!post_therapy_.sleephq_grace_armed) {
        post_therapy_.sleephq_grace_armed = true;
        post_therapy_.sleephq_due_ms =
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
        return;
    }
    if (post_therapy_.sleephq_due_ms == 0) {
        post_therapy_.sleephq_due_ms =
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
        return;
    }
    if (static_cast<int32_t>(now_ms - post_therapy_.sleephq_due_ms) >= 0) {
        if (sleephq_sync_->request_post_therapy_sync()) {
            clear_post_therapy_sleephq();
        } else {
            if (deadline_reached) {
                Log::logf(CAT_SLEEPHQ, LOG_WARN,
                          "post-therapy sync skipped after queue wait\n");
                clear_post_therapy_sleephq();
                return;
            }
            post_therapy_.sleephq_due_ms =
                due_after(now_ms, AC_BG_WORKER_BUSY_RECHECK_MS);
        }
    }
}

void ExportCoordinator::queue_post_therapy_storage_sync(uint32_t now_ms) {
    if (!storage_sync_) {
        post_therapy_.storage_pending = false;
        post_therapy_.storage_due_ms = 0;
        post_therapy_.storage_deadline_ms = 0;
        return;
    }
    if (storage_sync_->request_post_therapy_sync()) {
        post_therapy_.storage_pending = false;
        post_therapy_.storage_due_ms = 0;
        post_therapy_.storage_deadline_ms = 0;
    } else {
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_BG_WORKER_BUSY_RECHECK_MS);
    }
}

void ExportCoordinator::clear_post_therapy_sleephq() {
    post_therapy_.sleephq_pending = false;
    post_therapy_.sleephq_grace_armed = false;
    post_therapy_.sleephq_due_ms = 0;
    post_therapy_.sleephq_deadline_ms = 0;
}

void ExportCoordinator::maybe_queue_sleephq_startup_check(
    bool network_connected,
    bool storage_sync_active,
    SleepHqSyncRuntimeStatus status) {
    if (!sleephq_sync_ || !network_connected || storage_sync_active) return;
    if (!status.configured || status.config_generation == 0) {
        startup_check_.requested_generation = 0;
        startup_check_.completed_generation = 0;
        return;
    }
    if (status.check_complete()) {
        startup_check_.completed_generation = status.config_generation;
    }
    if (startup_check_.completed_generation == status.config_generation ||
        startup_check_.requested_generation == status.config_generation ||
        status.state == SleepHqSyncState::Pending ||
        status.state == SleepHqSyncState::Working) {
        return;
    }
    if (sleephq_sync_->request_check("startup_check")) {
        startup_check_.requested_generation = status.config_generation;
    }
}

void ExportCoordinator::poll_sleephq_idle_backfill(
    RpcArbiter &arbiter,
    ReportManager &report,
    bool network_connected,
    bool storage_sync_active,
    SleepHqSyncRuntimeStatus status,
    uint32_t now_ms) {
    if (!sleephq_sync_ || !network_connected) return;

    if (!status.configured || status.config_generation == 0) {
        clear_idle_backfill();
        return;
    }

    if (status.config_generation != idle_backfill_.queued_generation &&
        status.config_generation != idle_backfill_.armed_generation &&
        startup_check_.completed_generation == status.config_generation &&
        status.state == SleepHqSyncState::Idle) {
        idle_backfill_.pending = true;
        idle_backfill_.armed_generation = status.config_generation;
        idle_backfill_.due_ms = 0;
    }

    if (!idle_backfill_.pending) return;
    if (status.state != SleepHqSyncState::Idle) return;
    if (arbiter.stream_activity_active() ||
        report.foreground_busy() ||
        report.background_work_active() ||
        storage_sync_active ||
        arbiter.as11_state().therapy_state() == As11TherapyState::Running) {
        idle_backfill_.due_ms = 0;
        return;
    }

    if (idle_backfill_.due_ms == 0) {
        idle_backfill_.due_ms =
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
        return;
    }
    if (static_cast<int32_t>(now_ms - idle_backfill_.due_ms) < 0) return;

    if (sleephq_sync_->request_sync("idle_backfill")) {
        idle_backfill_.queued_generation = idle_backfill_.armed_generation;
        idle_backfill_.pending = false;
        idle_backfill_.due_ms = 0;
    }
}

void ExportCoordinator::clear_idle_backfill() {
    idle_backfill_.queued_generation = 0;
    idle_backfill_.armed_generation = 0;
    idle_backfill_.pending = false;
    idle_backfill_.due_ms = 0;
}

}  // namespace aircannect
