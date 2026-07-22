#include "export_coordinator.h"

#include "board_report.h"
#include "debug_log.h"

namespace aircannect {

uint32_t ExportCoordinator::due_after(uint32_t now_ms, uint32_t delay_ms) {
    uint32_t due = now_ms + delay_ms;
    if (due == 0) due = 1;
    return due;
}

void ExportCoordinator::begin(ExportTask &task) {
    task_ = &task;
}

ExportTaskStatus ExportCoordinator::status() const {
    return task_ ? task_->status() : ExportTaskStatus();
}

StorageSyncStatus ExportCoordinator::smb_status() const {
    return status().smb;
}

SleepHqSyncStatus ExportCoordinator::sleephq_status() const {
    return status().sleephq;
}

bool ExportCoordinator::endpoint_work_active() const {
    return status().active;
}

bool ExportCoordinator::request_smb_sync() {
    if (!task_) return false;
    const ExportTaskStatus current = status();
    if (current.busy || !task_->request_smb_sync()) return false;

    startup_check_.smb_requested_generation =
        current.smb.config_generation;
    return true;
}

bool ExportCoordinator::request_smb_verify() {
    if (!task_) return false;
    const ExportTaskStatus current = status();
    if (current.busy || !task_->request_smb_verify()) return false;

    startup_check_.smb_requested_generation =
        current.smb.config_generation;
    return true;
}

bool ExportCoordinator::request_sleephq_sync() {
    if (!task_ || status().busy) return false;
    return task_->request_sleephq_sync();
}

bool ExportCoordinator::request_sleephq_sync_day(const char *day) {
    if (!task_ || status().busy) return false;
    return task_->request_sleephq_sync_day(day);
}

bool ExportCoordinator::request_sleephq_check() {
    if (!task_ || status().busy) return false;
    return task_->request_sleephq_check();
}

void ExportCoordinator::poll(const ExportReportActivity &report,
                             const ActivitySnapshot &activity,
                             uint32_t now_ms) {
    if (!task_) return;

    const ExportTaskStatus task_status = task_->status();
    const StorageSyncRuntimeStatus storage = task_status.smb_runtime;
    const SleepHqSyncRuntimeStatus sleephq = task_status.sleephq_runtime;
    const bool storage_active = storage.active();

    poll_post_therapy(report,
                      activity.realtime_stream_active,
                      activity.therapy_active,
                      storage_active,
                      now_ms);

    if (sleephq.state == SleepHqSyncState::Working) {
        task_->defer_smb_until(
            due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS));
    }

    const bool startup_idle =
        startup_idle_work_allowed(now_ms) &&
        !report.foreground_active && !report.background_active &&
        !activity.therapy_active && !activity.realtime_stream_active &&
        !activity.ota_install_active;
    if (startup_idle) {
        maybe_queue_smb_startup_check(task_status.network_ready,
                                      task_status.smb,
                                      sleephq);
        maybe_queue_sleephq_startup_check(task_status.network_ready,
                                          storage_active,
                                          sleephq);
    }
    poll_sleephq_idle_backfill(report,
                               task_status.network_ready,
                               activity.realtime_stream_active,
                               activity.therapy_active,
                               storage_active,
                               sleephq,
                               now_ms);
}

void ExportCoordinator::poll_post_therapy(
    const ExportReportActivity &report,
    bool stream_activity_active,
    bool therapy_active,
    bool storage_sync_active,
    uint32_t now_ms) {
    if (!post_therapy_.state_initialized) {
        post_therapy_.state_initialized = true;
        post_therapy_.last_therapy_active = therapy_active;
    }

    if (therapy_active) {
        reset_post_therapy_after_running();
        post_therapy_.last_therapy_active = true;
        return;
    }

    if (post_therapy_.last_therapy_active) {
        arm_post_therapy_after_stop(now_ms);
    }

    maybe_finish_report_settle(report, stream_activity_active, now_ms);
    maybe_queue_storage_sync(report, stream_activity_active, now_ms);
    maybe_queue_post_therapy_sleephq(report,
                                     stream_activity_active,
                                     storage_sync_active,
                                     now_ms);
    post_therapy_.last_therapy_active = false;
}

void ExportCoordinator::reset_post_therapy_after_running() {
    post_therapy_.report_settle_due_ms = 0;
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
    const ExportTaskStatus task_status = status();

    post_therapy_.report_settle_due_ms =
        due_after(now_ms, AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS);
    post_therapy_.storage_pending = task_ && task_status.smb_runtime.enabled &&
                                    task_status.smb_runtime.configured;
    post_therapy_.sleephq_pending =
        task_ && task_status.sleephq_runtime.configured;
    post_therapy_.storage_grace_armed = false;
    post_therapy_.sleephq_grace_armed = false;
    post_therapy_.storage_due_ms = 0;
    post_therapy_.sleephq_due_ms = 0;
    post_therapy_.storage_deadline_ms =
        due_after(now_ms, AC_REPORT_POST_THERAPY_SYNC_MAX_WAIT_MS);
    post_therapy_.sleephq_deadline_ms =
        due_after(now_ms, AC_SLEEPHQ_POST_THERAPY_SYNC_MAX_WAIT_MS);

    if (task_) task_->defer_smb_until(post_therapy_.report_settle_due_ms);
}

void ExportCoordinator::maybe_finish_report_settle(
    const ExportReportActivity &report,
    bool stream_activity_active,
    uint32_t now_ms) {
    if (post_therapy_.report_settle_due_ms == 0 ||
        static_cast<int32_t>(now_ms -
                             post_therapy_.report_settle_due_ms) < 0) {
        return;
    }
    if (stream_activity_active || report.background_active) {
        if (task_) {
            task_->defer_smb_until(
                due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS));
        }
        return;
    }

    post_therapy_.report_settle_due_ms = 0;
    post_therapy_.storage_grace_armed = false;
    post_therapy_.storage_due_ms = 0;
}

void ExportCoordinator::maybe_queue_storage_sync(
    const ExportReportActivity &report,
    bool stream_activity_active,
    uint32_t now_ms) {
    if (!post_therapy_.storage_pending) return;

    if (!task_) {
        post_therapy_.storage_pending = false;
        post_therapy_.storage_due_ms = 0;
        post_therapy_.storage_deadline_ms = 0;
        return;
    }
    if (post_therapy_.report_settle_due_ms != 0) {
        const bool deadline_reached =
            post_therapy_.storage_deadline_ms != 0 &&
            static_cast<int32_t>(now_ms -
                                 post_therapy_.storage_deadline_ms) >= 0;
        if (deadline_reached) {
            Log::logf(CAT_STORAGE, LOG_WARN,
                      "[SYNC] post-therapy sync fallback after report wait\n");
            queue_post_therapy_storage_sync(now_ms);
        }
        return;
    }
    if (stream_activity_active) {
        post_therapy_.storage_due_ms = 0;
        task_->defer_smb_until(
            due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS));
        return;
    }
    if (report.background_active) {
        const bool deadline_reached =
            post_therapy_.storage_deadline_ms != 0 &&
            static_cast<int32_t>(now_ms -
                                 post_therapy_.storage_deadline_ms) >= 0;
        if (!deadline_reached) {
            post_therapy_.storage_due_ms = 0;
            task_->defer_smb_until(
                due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS));
            return;
        }

        Log::logf(CAT_STORAGE, LOG_WARN,
                  "[SYNC] post-therapy sync fallback after report wait\n");
        queue_post_therapy_storage_sync(now_ms);
        return;
    }
    if (!post_therapy_.storage_grace_armed) {
        post_therapy_.storage_grace_armed = true;
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
        task_->defer_smb_until(post_therapy_.storage_due_ms);
        return;
    }
    if (post_therapy_.storage_due_ms == 0) {
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
        task_->defer_smb_until(post_therapy_.storage_due_ms);
        return;
    }
    if (static_cast<int32_t>(now_ms - post_therapy_.storage_due_ms) >= 0) {
        queue_post_therapy_storage_sync(now_ms);
    }
}

void ExportCoordinator::maybe_queue_post_therapy_sleephq(
    const ExportReportActivity &report,
    bool stream_activity_active,
    bool storage_sync_active,
    uint32_t now_ms) {
    if (!post_therapy_.sleephq_pending) return;

    const bool deadline_reached =
        post_therapy_.sleephq_deadline_ms != 0 &&
        static_cast<int32_t>(now_ms -
                             post_therapy_.sleephq_deadline_ms) >= 0;
    if (post_therapy_.report_settle_due_ms != 0) {
        if (deadline_reached) {
            Log::logf(CAT_SLEEPHQ, LOG_WARN,
                      "post-therapy sync skipped after report wait\n");
            clear_post_therapy_sleephq();
        }
        return;
    }
    if (!task_) {
        clear_post_therapy_sleephq();
        return;
    }

    const bool storage_blocking = post_therapy_.storage_pending ||
                                  storage_sync_active;
    if (stream_activity_active || report.background_active ||
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
            due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
        return;
    }
    if (post_therapy_.sleephq_due_ms == 0) {
        post_therapy_.sleephq_due_ms =
            due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
        return;
    }
    if (static_cast<int32_t>(now_ms - post_therapy_.sleephq_due_ms) < 0) {
        return;
    }

    if (task_->request_sleephq_post_therapy()) {
        clear_post_therapy_sleephq();
    } else if (deadline_reached) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "post-therapy sync skipped after queue wait\n");
        clear_post_therapy_sleephq();
    } else {
        post_therapy_.sleephq_due_ms =
            due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS);
    }
}

void ExportCoordinator::queue_post_therapy_storage_sync(uint32_t now_ms) {
    if (!task_) {
        post_therapy_.storage_pending = false;
        post_therapy_.storage_due_ms = 0;
        post_therapy_.storage_deadline_ms = 0;
        return;
    }

    if (task_->request_smb_post_therapy()) {
        post_therapy_.storage_pending = false;
        post_therapy_.storage_due_ms = 0;
        post_therapy_.storage_deadline_ms = 0;
    } else {
        post_therapy_.storage_due_ms =
            due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS);
    }
}

void ExportCoordinator::clear_post_therapy_sleephq() {
    post_therapy_.sleephq_pending = false;
    post_therapy_.sleephq_grace_armed = false;
    post_therapy_.sleephq_due_ms = 0;
    post_therapy_.sleephq_deadline_ms = 0;
}

bool ExportCoordinator::startup_idle_work_allowed(uint32_t now_ms) {
    if (startup_check_.idle_grace_complete) return true;
    if (static_cast<int32_t>(
            now_ms - AC_RUNTIME_STARTUP_IDLE_GRACE_MS) < 0) {
        return false;
    }

    startup_check_.idle_grace_complete = true;
    return true;
}

void ExportCoordinator::maybe_queue_smb_startup_check(
    bool network_connected,
    const StorageSyncStatus &status,
    SleepHqSyncRuntimeStatus sleephq) {
    if (!task_ || !network_connected) return;
    if (!status.enabled || !status.configured ||
        status.config_generation == 0) {
        startup_check_.smb_requested_generation = 0;
        return;
    }
    if (startup_check_.smb_requested_generation ==
            status.config_generation ||
        status.pending || status.state == StorageSyncState::Working ||
        sleephq.pending || sleephq.state == SleepHqSyncState::Working) {
        return;
    }

    if (task_->request_smb_startup_check()) {
        startup_check_.smb_requested_generation = status.config_generation;
    }
}

void ExportCoordinator::maybe_queue_sleephq_startup_check(
    bool network_connected,
    bool storage_sync_active,
    SleepHqSyncRuntimeStatus status) {
    if (!task_ || !network_connected || storage_sync_active) return;
    if (!status.configured || status.config_generation == 0) {
        startup_check_.sleephq_requested_generation = 0;
        startup_check_.sleephq_completed_generation = 0;
        return;
    }
    if (status.check_complete()) {
        startup_check_.sleephq_completed_generation =
            status.config_generation;
    }
    if (startup_check_.sleephq_completed_generation ==
            status.config_generation ||
        startup_check_.sleephq_requested_generation ==
            status.config_generation ||
        status.state == SleepHqSyncState::Pending ||
        status.state == SleepHqSyncState::Working) {
        return;
    }

    if (task_->request_sleephq_startup_check()) {
        startup_check_.sleephq_requested_generation =
            status.config_generation;
    }
}

void ExportCoordinator::poll_sleephq_idle_backfill(
    const ExportReportActivity &report,
    bool network_connected,
    bool stream_activity_active,
    bool therapy_active,
    bool storage_sync_active,
    SleepHqSyncRuntimeStatus status,
    uint32_t now_ms) {
    if (!task_ || !network_connected) return;

    if (!status.configured || status.config_generation == 0) {
        clear_idle_backfill();
        return;
    }

    if (status.config_generation != idle_backfill_.queued_generation &&
        status.config_generation != idle_backfill_.armed_generation &&
        startup_check_.sleephq_completed_generation ==
            status.config_generation &&
        status.state == SleepHqSyncState::Idle) {
        idle_backfill_.pending = true;
        idle_backfill_.armed_generation = status.config_generation;
        idle_backfill_.due_ms = 0;
    }

    if (!idle_backfill_.pending || status.state != SleepHqSyncState::Idle) {
        return;
    }
    if (stream_activity_active || report.foreground_active ||
        report.background_active || storage_sync_active || therapy_active) {
        idle_backfill_.due_ms = 0;
        return;
    }

    if (idle_backfill_.due_ms == 0) {
        idle_backfill_.due_ms =
            due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
        return;
    }
    if (static_cast<int32_t>(now_ms - idle_backfill_.due_ms) < 0) return;

    if (task_->request_sleephq_idle_backfill()) {
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
