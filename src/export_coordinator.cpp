#include "export_coordinator.h"

#include "board_report.h"
#include "debug_log.h"

namespace aircannect {

uint32_t ExportCoordinator::due_after(uint32_t now_ms, uint32_t delay_ms) {
    uint32_t due = now_ms + delay_ms;
    if (due == 0) due = 1;
    return due;
}

void ExportCoordinator::poll(RpcArbiter &arbiter,
                             ReportManager &report,
                             StorageSyncJob *storage_sync,
                             SleepHqSyncJob *sleephq_sync,
                             const AppConfigData &config,
                             bool network_connected,
                             bool resmed_ota_active,
                             bool esp_ota_active,
                             uint32_t now_ms) {
    if (storage_sync) {
        storage_sync->set_network_available(network_connected);
        storage_sync->refresh_config(config, now_ms);
    }

    if (sleephq_sync) {
        sleephq_sync->set_network_available(network_connected);
        sleephq_sync->refresh_config(config, now_ms);
    }

    poll_post_therapy(arbiter, report, storage_sync, sleephq_sync, now_ms);

    const bool sleephq_working =
        sleephq_sync &&
        sleephq_sync->status().state == SleepHqSyncState::Working;
    if (storage_sync && sleephq_working) {
        storage_sync->defer_idle_work_until(
            now_ms + AC_BG_WORKER_BUSY_RECHECK_MS);
    }

    const bool storage_sync_active =
        storage_sync && storage_sync->active();
    if (sleephq_sync) {
        sleephq_sync->set_runtime_blocked(
            report.foreground_busy() ||
            storage_sync_active ||
            arbiter.stream_activity_active() ||
            resmed_ota_active ||
            esp_ota_active ||
            arbiter.as11_state().therapy_state() ==
                As11TherapyState::Running);
    }

    maybe_queue_sleephq_startup_check(sleephq_sync,
                                      network_connected,
                                      storage_sync_active);
    poll_sleephq_idle_backfill(arbiter,
                               report,
                               storage_sync,
                               sleephq_sync,
                               network_connected,
                               storage_sync_active,
                               now_ms);
}

void ExportCoordinator::poll_post_therapy(RpcArbiter &arbiter,
                                          ReportManager &report,
                                          StorageSyncJob *storage_sync,
                                          SleepHqSyncJob *sleephq_sync,
                                          uint32_t now_ms) {
    const As11TherapyState state = arbiter.as11_state().therapy_state();

    if (state == As11TherapyState::Running) {
        reset_post_therapy_after_running();
        post_therapy_.last_state = state;
        return;
    }

    if (post_therapy_.last_state == As11TherapyState::Running &&
        state != As11TherapyState::Running) {
        arm_post_therapy_after_stop(storage_sync, sleephq_sync, now_ms);
    }

    maybe_refresh_summary(arbiter, report, storage_sync, now_ms);
    maybe_queue_storage_sync(arbiter, report, storage_sync, now_ms);
    maybe_queue_post_therapy_sleephq(arbiter,
                                     report,
                                     storage_sync,
                                     sleephq_sync,
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
}

void ExportCoordinator::arm_post_therapy_after_stop(
    StorageSyncJob *storage_sync,
    SleepHqSyncJob *sleephq_sync,
    uint32_t now_ms) {
    post_therapy_.summary_refresh_due_ms =
        due_after(now_ms, AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS);
    post_therapy_.storage_pending = storage_sync != nullptr;
    post_therapy_.sleephq_pending = sleephq_sync != nullptr;
    post_therapy_.storage_grace_armed = false;
    post_therapy_.sleephq_grace_armed = false;
    post_therapy_.storage_due_ms = 0;
    post_therapy_.sleephq_due_ms = 0;
    post_therapy_.storage_deadline_ms =
        due_after(now_ms, AC_REPORT_POST_THERAPY_SYNC_MAX_WAIT_MS);
    if (storage_sync) {
        storage_sync->defer_idle_work_until(
            post_therapy_.summary_refresh_due_ms);
    }
}

void ExportCoordinator::maybe_refresh_summary(RpcArbiter &arbiter,
                                              ReportManager &report,
                                              StorageSyncJob *storage_sync,
                                              uint32_t now_ms) {
    if (post_therapy_.summary_refresh_due_ms == 0 ||
        static_cast<int32_t>(now_ms -
                             post_therapy_.summary_refresh_due_ms) < 0) {
        return;
    }
    if (arbiter.stream_activity_active()) {
        post_therapy_.summary_refresh_due_ms =
            due_after(now_ms, AC_BG_WORKER_BUSY_RECHECK_MS);
        if (storage_sync) {
            storage_sync->defer_idle_work_until(
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
                                                 StorageSyncJob *storage_sync,
                                                 uint32_t now_ms) {
    if (!post_therapy_.storage_pending ||
        post_therapy_.summary_refresh_due_ms != 0) {
        return;
    }

    if (!storage_sync) {
        post_therapy_.storage_pending = false;
        post_therapy_.storage_due_ms = 0;
        post_therapy_.storage_deadline_ms = 0;
        return;
    }
    if (arbiter.stream_activity_active()) {
        post_therapy_.storage_due_ms = 0;
        storage_sync->defer_idle_work_until(
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
            storage_sync->defer_idle_work_until(
                due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS));
            return;
        }
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SYNC] post-therapy sync fallback after report wait\n");
        queue_post_therapy_storage_sync(storage_sync);
        return;
    }
    if (!post_therapy_.storage_grace_armed) {
        post_therapy_.storage_grace_armed = true;
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
        storage_sync->defer_idle_work_until(post_therapy_.storage_due_ms);
        return;
    }
    if (post_therapy_.storage_due_ms == 0) {
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
        storage_sync->defer_idle_work_until(post_therapy_.storage_due_ms);
        return;
    }
    if (static_cast<int32_t>(now_ms - post_therapy_.storage_due_ms) >= 0) {
        queue_post_therapy_storage_sync(storage_sync);
    }
}

void ExportCoordinator::maybe_queue_post_therapy_sleephq(
    RpcArbiter &arbiter,
    ReportManager &report,
    StorageSyncJob *storage_sync,
    SleepHqSyncJob *sleephq_sync,
    uint32_t now_ms) {
    if (!post_therapy_.sleephq_pending ||
        post_therapy_.summary_refresh_due_ms != 0 ||
        post_therapy_.storage_pending) {
        return;
    }
    if (!sleephq_sync) {
        post_therapy_.sleephq_pending = false;
        post_therapy_.sleephq_due_ms = 0;
        return;
    }
    if (arbiter.stream_activity_active() ||
        report.background_work_active() ||
        (storage_sync && storage_sync->active())) {
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
        (void)sleephq_sync->request_sync("post_therapy");
        post_therapy_.sleephq_pending = false;
        post_therapy_.sleephq_due_ms = 0;
    }
}

void ExportCoordinator::queue_post_therapy_storage_sync(
    StorageSyncJob *storage_sync) {
    (void)storage_sync->request_post_therapy_sync();
    post_therapy_.storage_pending = false;
    post_therapy_.storage_due_ms = 0;
    post_therapy_.storage_deadline_ms = 0;
}

void ExportCoordinator::maybe_queue_sleephq_startup_check(
    SleepHqSyncJob *sleephq_sync,
    bool network_connected,
    bool storage_sync_active) {
    if (!sleephq_sync || !network_connected || storage_sync_active) return;
    const SleepHqSyncStatus status = sleephq_sync->status();
    if (!status.configured || status.config_generation == 0) {
        startup_check_.requested_generation = 0;
        startup_check_.completed_generation = 0;
        return;
    }
    if (startup_check_.requested_generation == status.config_generation &&
        status.state == SleepHqSyncState::Idle &&
        status.team_id != 0 &&
        status.last_check_epoch != 0) {
        startup_check_.completed_generation = status.config_generation;
    }
    if (startup_check_.completed_generation == status.config_generation ||
        startup_check_.requested_generation == status.config_generation ||
        status.state == SleepHqSyncState::Pending ||
        status.state == SleepHqSyncState::Working) {
        return;
    }
    if (sleephq_sync->request_check("startup_check")) {
        startup_check_.requested_generation = status.config_generation;
    }
}

void ExportCoordinator::poll_sleephq_idle_backfill(
    RpcArbiter &arbiter,
    ReportManager &report,
    StorageSyncJob *storage_sync,
    SleepHqSyncJob *sleephq_sync,
    bool network_connected,
    bool storage_sync_active,
    uint32_t now_ms) {
    if (!sleephq_sync || !network_connected) return;

    const SleepHqSyncStatus status = sleephq_sync->status();
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
        (storage_sync && storage_sync->active()) ||
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

    if (sleephq_sync->request_sync("idle_backfill")) {
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
