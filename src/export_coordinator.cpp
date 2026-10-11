#include "export_coordinator.h"

#include "board_report.h"
#include "debug_log.h"
#include "storage_export_plan.h"

namespace aircannect {

uint32_t ExportCoordinator::due_after(uint32_t now_ms, uint32_t delay_ms) {
    uint32_t due = now_ms + delay_ms;
    if (due == 0) due = 1;
    return due;
}

void ExportCoordinator::begin(ExportTask &task) {
    task_ = &task;
}

ExportTaskControlSnapshot ExportCoordinator::control_snapshot() const {
    return task_ ? task_->control_snapshot() : ExportTaskControlSnapshot();
}

StorageSyncStatus ExportCoordinator::smb_status() const {
    return smb_snapshot().sync;
}

SleepHqSyncStatus ExportCoordinator::sleephq_status() const {
    return sleephq_snapshot().sync;
}

ExportStatusSnapshot ExportCoordinator::endpoint_status_snapshot() const {
    return task_ ? task_->status_snapshot() : ExportStatusSnapshot();
}

ExportSmbStatusSnapshot ExportCoordinator::smb_snapshot() const {
    return task_ ? task_->smb_status() : ExportSmbStatusSnapshot();
}

ExportSleepHqStatusSnapshot ExportCoordinator::sleephq_snapshot() const {
    return task_ ? task_->sleephq_status()
                 : ExportSleepHqStatusSnapshot();
}

ExportCoordinator::EndpointSchedule &ExportCoordinator::schedule(ExportEndpoint id) {
    return endpoints_[static_cast<size_t>(id)];
}

const ExportCoordinator::EndpointSchedule &ExportCoordinator::schedule(
    ExportEndpoint id) const {
    return endpoints_[static_cast<size_t>(id)];
}

bool ExportCoordinator::check_settled(
    ExportEndpoint id, const ExportTaskControlSnapshot &status) const {
    const auto &current = status.endpoint(id);
    if (!current.configured || !current.definition->requires_check()) return true;
    if (current.check_succeeded()) return true;

    // A failed attempt releases ordering, but never authorizes backfill.
    return current.config_generation != 0 &&
           schedule(id).check_requested_generation == current.config_generation &&
           !status.command_pending && !current.busy() &&
           current.state == ExportState::Error;
}

bool ExportCoordinator::endpoint_work_claimed() const {
    if (!task_) return false;
    if (task_->endpoint_work_claimed()) return true;

    const auto status = control_snapshot();
    for (const auto id : status.order) {
        if (schedule(id).check_requested_generation != 0 &&
            !check_settled(id, status)) return true;
    }
    return false;
}

void ExportCoordinator::record_request(
    const ExportRequest &work, const ExportEndpointStatus &status) {
    auto &state = schedule(work.endpoint);
    const bool full_sync = work.operation != ExportOperation::Check &&
                           work.reason != ExportReason::Maintenance &&
                           !work.day[0];

    if (work.operation == ExportOperation::Check ||
        (full_sync && status.definition->sync_confirms_readiness)) {
        state.check_requested_generation = status.config_generation;
    }
    if (full_sync) state.backfill_queued_generation = status.config_generation;
}

bool ExportCoordinator::request(const ExportRequest &work) {
    if (!task_) return false;

    const auto current = control_snapshot();
    const ExportBackendDefinition *definition = nullptr;
    for (const auto id : current.order) {
        if (id == work.endpoint) definition = current.endpoint(id).definition;
    }
    if (!definition || !work.supported(*definition)) return false;

    const auto *rule = definition->rule(work.operation, work.reason);
    if (!current.accepts(rule->work) || !task_->request(work)) return false;

    if (work.operation != ExportOperation::Check) {
        record_request(work, current.endpoint(work.endpoint));
    }
    return true;
}

bool ExportCoordinator::request_sync() {
    if (!task_) return false;
    for (const auto &state : endpoints_) {
        if (state.manual_pending) return false;
    }

    const auto current = control_snapshot();
    bool queued = false;
    for (const auto id : current.order) {
        const auto &endpoint = current.endpoint(id);
        if (!endpoint.configured ||
            !endpoint.definition->rule(ExportOperation::Sync, ExportReason::Manual)) {
            continue;
        }

        if (!queued) {
            if (!request({id, ExportOperation::Sync, ExportReason::Manual})) return false;
            queued = true;
        } else {
            schedule(id).manual_pending = true;
        }
    }
    return queued;
}

void ExportCoordinator::defer_idle_work(
    const ExportTaskControlSnapshot &status, uint32_t until_ms) {
    for (const auto id : status.order) task_->defer_idle_until(id, until_ms);
}

void ExportCoordinator::poll(const ExportReportActivity &report,
                             const ActivitySnapshot &activity,
                             uint32_t now_ms) {
    if (!task_) return;

    const auto status = task_->control_snapshot();
    status_snapshot_ = status;

    poll_post_therapy(report, activity.realtime_stream_active,
                      activity.therapy_active, status, now_ms);
    poll_manual_sync(status);
    poll_maintenance(report, activity, status, now_ms);

    for (const auto active : status.order) {
        const auto &work = status.endpoint(active);
        if (!work.executing()) continue;

        for (const auto other : status.order) {
            if (other != active &&
                (work.work.exclusive || status.endpoint(other).work.exclusive)) {
                task_->defer_idle_until(
                    other, due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS));
            }
        }
    }

    const bool startup_idle =
        startup_idle_work_allowed(now_ms) && !report.foreground_active &&
        !activity.therapy_active && !activity.realtime_stream_active &&
        !activity.ota_install_active;

    if (startup_idle) queue_startup_checks(status);
    poll_startup_backfill(report, activity, status, now_ms);
}

void ExportCoordinator::poll_manual_sync(const ExportTaskControlSnapshot &status) {
    if (status.command_pending || status.busy) return;

    for (const auto id : status.order) {
        auto &state = schedule(id);
        if (!state.manual_pending) continue;

        if (request({id, ExportOperation::Sync, ExportReason::Manual})) {
            state.manual_pending = false;
        }
        return;
    }
}

void ExportCoordinator::poll_post_therapy(
    const ExportReportActivity &report, bool stream_activity_active,
    bool therapy_active, const ExportTaskControlSnapshot &status, uint32_t now_ms) {
    if (!post_therapy_.state_initialized) {
        post_therapy_.last_therapy_active = therapy_active;
        post_therapy_.state_initialized = true;
    }
    if (therapy_active && !post_therapy_.last_therapy_active) {
        post_therapy_.cancel_pending = true;
    }
    if (post_therapy_.cancel_pending && task_->cancel_post_therapy()) {
        post_therapy_.cancel_pending = false;
    }

    if (therapy_active) {
        reset_post_therapy_after_running();
        post_therapy_.last_therapy_active = true;
        return;
    }
    if (post_therapy_.cancel_pending) return;

    if (post_therapy_.last_therapy_active) arm_post_therapy_after_stop(now_ms);

    maybe_finish_report_settle(report, stream_activity_active, now_ms);
    queue_post_therapy(stream_activity_active, status, now_ms);
    post_therapy_.last_therapy_active = false;
}

void ExportCoordinator::reset_post_therapy_after_running() {
    post_therapy_.report_settle_due_ms = 0;
    post_therapy_.report_settle_deadline_ms = 0;
    for (auto &state : endpoints_) {
        state.post_therapy_pending = false;
        state.post_therapy_due_ms = 0;
    }
}

void ExportCoordinator::arm_post_therapy_after_stop(uint32_t now_ms) {
    const auto status = control_snapshot();
    bool pending = false;
    for (const auto id : status.order) {
        const auto &endpoint = status.endpoint(id);
        auto &state = schedule(id);
        state.post_therapy_pending = endpoint.configured &&
            endpoint.definition->rule(ExportOperation::Sync, ExportReason::PostTherapy);
        state.post_therapy_due_ms = 0;
        pending = pending || state.post_therapy_pending;
    }

    post_therapy_.report_settle_due_ms =
        pending ? due_after(now_ms, AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS) : 0;
    post_therapy_.report_settle_deadline_ms =
        pending ? due_after(now_ms, AC_EXPORT_POST_THERAPY_SETTLE_MAX_WAIT_MS) : 0;

    if (pending) defer_idle_work(status, post_therapy_.report_settle_due_ms);
}

void ExportCoordinator::maybe_finish_report_settle(
    const ExportReportActivity &report, bool stream_activity_active, uint32_t now_ms) {
    if (post_therapy_.report_settle_due_ms == 0 ||
        static_cast<int32_t>(now_ms - post_therapy_.report_settle_due_ms) < 0) {
        return;
    }

    const bool deadline_reached = post_therapy_.report_settle_deadline_ms != 0 &&
        static_cast<int32_t>(now_ms - post_therapy_.report_settle_deadline_ms) >= 0;

    if (stream_activity_active || (report.post_therapy_settle_pending && !deadline_reached)) {
        defer_idle_work(control_snapshot(), due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS));
        return;
    }

    if (report.post_therapy_settle_pending) {
        Log::logf(CAT_EXPORT, LOG_WARN,
                  "post-therapy exports proceeding after report wait\n");
    }

    post_therapy_.report_settle_due_ms = 0;
    post_therapy_.report_settle_deadline_ms = 0;
}

void ExportCoordinator::queue_post_therapy(
    bool stream_activity_active, const ExportTaskControlSnapshot &status, uint32_t now_ms) {
    if (post_therapy_.report_settle_due_ms != 0) return;

    bool previous_active = false;
    for (const auto id : status.order) {
        auto &state = schedule(id);
        if (state.post_therapy_pending) {
            if (stream_activity_active || previous_active) {
                state.post_therapy_due_ms = 0;
                task_->defer_idle_until(id, due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS));
            } else if (state.post_therapy_due_ms == 0) {
                state.post_therapy_due_ms = due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
                task_->defer_idle_until(id, state.post_therapy_due_ms);
            } else if (static_cast<int32_t>(now_ms - state.post_therapy_due_ms) >= 0) {
                const auto current = control_snapshot();
                const ExportRequest work{id, ExportOperation::Sync, ExportReason::PostTherapy};
                if (task_->request(work)) {
                    record_request(work, current.endpoint(id));
                    state.post_therapy_pending = false;
                    state.post_therapy_due_ms = 0;
                } else {
                    state.post_therapy_due_ms = due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS);
                }
            }
        }

        previous_active = previous_active || state.post_therapy_pending ||
                          status.endpoint(id).active();
    }
}

bool ExportCoordinator::post_therapy_pending() const {
    if (post_therapy_.report_settle_due_ms != 0) return true;
    for (const auto &state : endpoints_) {
        if (state.post_therapy_pending) return true;
    }
    return false;
}

bool ExportCoordinator::startup_idle_work_allowed(uint32_t now_ms) {
    if (startup_idle_grace_complete_) return true;
    if (static_cast<int32_t>(now_ms - AC_RUNTIME_STARTUP_IDLE_GRACE_MS) < 0) return false;

    startup_idle_grace_complete_ = true;
    return true;
}

void ExportCoordinator::queue_startup_checks(const ExportTaskControlSnapshot &status) {
    if (!status.network_ready) return;

    for (const auto id : status.order) {
        const auto &current = status.endpoint(id);
        auto &state = schedule(id);
        if (!current.configured) {
            state.check_requested_generation = 0;
            continue;
        }

        if (current.definition->requires_check() && current.config_generation != 0 &&
            !current.check_succeeded() &&
            state.check_requested_generation != current.config_generation) {
            if (status.busy) return;

            const ExportRequest work{id, ExportOperation::Check, ExportReason::Startup};
            if (task_->request(work)) record_request(work, current);
            return;
        }

        if (!check_settled(id, status) || current.active()) return;
    }
}

void ExportCoordinator::poll_startup_backfill(
    const ExportReportActivity &report, const ActivitySnapshot &activity,
    const ExportTaskControlSnapshot &status, uint32_t now_ms) {
    for (const auto id : status.order) {
        if (!status.endpoint(id).configured) schedule(id).backfill_queued_generation = 0;
        if (!check_settled(id, status)) {
            startup_backfill_due_ms_ = 0;
            return;
        }
    }

    const bool busy = !status.network_ready || task_->endpoint_work_claimed() ||
        status.busy || report.foreground_active || report.background_active ||
        activity.therapy_active || activity.realtime_stream_active || activity.ota_install_active;

    for (const auto id : status.order) {
        const auto &current = status.endpoint(id);
        const bool ready = current.configured && current.ready() && current.backfill_ready() &&
            current.definition->rule(ExportOperation::Sync, ExportReason::IdleBackfill) &&
            schedule(id).backfill_queued_generation != current.config_generation;
        if (!ready || busy) continue;

        if (startup_backfill_due_ms_ == 0) {
            startup_backfill_due_ms_ = due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
            return;
        }
        if (static_cast<int32_t>(now_ms - startup_backfill_due_ms_) < 0) return;

        const ExportRequest work{id, ExportOperation::Sync, ExportReason::IdleBackfill};
        if (task_->request(work)) {
            record_request(work, current);
            startup_backfill_due_ms_ = 0;
        }
        return;
    }
    startup_backfill_due_ms_ = 0;
}

bool ExportCoordinator::startup_backfill_pending(const ExportTaskControlSnapshot &status) const {
    for (const auto id : status.order) {
        const auto &current = status.endpoint(id);
        if (current.configured && current.backfill_ready() &&
            current.definition->rule(ExportOperation::Sync, ExportReason::IdleBackfill) &&
            schedule(id).backfill_queued_generation != current.config_generation) return true;
    }
    return false;
}

void ExportCoordinator::poll_maintenance(
    const ExportReportActivity &report, const ActivitySnapshot &activity,
    const ExportTaskControlSnapshot &status, uint32_t now_ms) {
    bool priority_work = !status.network_ready || report.foreground_active ||
        report.background_active || activity.therapy_active || activity.realtime_stream_active ||
        activity.ota_install_active || post_therapy_pending() || startup_backfill_pending(status);

    for (const auto id : status.order) {
        if (!check_settled(id, status)) priority_work = true;
    }

    for (const auto id : status.order) {
        const auto &current = status.endpoint(id);
        const auto *rule = current.definition->rule(ExportOperation::Reconcile,
                                                    ExportReason::Maintenance);
        if (!rule) continue;

        auto &state = schedule(id);
        if (state.maintenance_generation != current.config_generation) {
            state.maintenance_generation = current.config_generation;
            state.maintenance_due_ms = 0;
            state.maintenance_queued = false;
        }

        bool peers_busy = false;
        bool peers_preempt = false;
        for (const auto other : status.order) {
            const auto &peer = status.endpoint(other);
            if (other == id || !peer.active()) continue;

            peers_busy = true;
            peers_preempt = peers_preempt || current.can_preempt(peer.work);
        }

        if (current.maintenance_pending) {
            if ((priority_work && current.can_preempt({})) || peers_preempt) {
                state.maintenance_due_ms = 0;
                task_->cancel_maintenance(id);
            }
            continue;
        }

        if (state.maintenance_queued) {
            if (status.command_pending || current.active()) continue;
            state.maintenance_queued = false;
            state.maintenance_due_ms = 0;
        }

        const uint64_t now_epoch = storage_export_current_epoch_seconds_or_zero();
        const auto interval = current.definition->maintenance_interval_seconds;
        const bool due = interval != 0 && now_epoch != 0 &&
            (current.last_maintenance_epoch == 0 ||
             now_epoch >= current.last_maintenance_epoch + interval);

        if (!due || priority_work || peers_busy || status.command_pending ||
            !current.configured || !current.ready() || !current.backfill_ready()) {
            state.maintenance_due_ms = 0;
            continue;
        }

        if (state.maintenance_due_ms == 0) {
            state.maintenance_due_ms = due_after(now_ms, AC_EXPORT_FULL_RECONCILE_IDLE_GRACE_MS);
        } else if (static_cast<int32_t>(now_ms - state.maintenance_due_ms) >= 0) {
            if (task_->request({id, ExportOperation::Reconcile, ExportReason::Maintenance})) {
                state.maintenance_queued = true;
                state.maintenance_due_ms = 0;
            }
        }
    }
}

}  // namespace aircannect
