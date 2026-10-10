#include "export_coordinator.h"

#include <string.h>

#include "board_report.h"
#include "debug_log.h"
#include "storage_export_plan.h"

namespace aircannect {

uint32_t ExportCoordinator::due_after(uint32_t now_ms, uint32_t delay_ms) {
    uint32_t due = now_ms + delay_ms;
    if (due == 0) due = 1;
    return due;
}

bool ExportCoordinator::external_request_allowed(
    const ExportTaskControlSnapshot &status) {
    if (status.command_pending) return false;
    if (!status.busy) return true;

    return status.endpoint(ExportEndpoint::Smb).scheduled_reconcile &&
           !status.endpoint(ExportEndpoint::SleepHq).active();
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

bool ExportCoordinator::endpoint_work_claimed() const {
    if (!task_) return false;
    if (task_->endpoint_work_claimed()) return true;

    const auto &smb = schedule(ExportEndpoint::Smb);
    return smb.check_requested_generation != 0 &&
           smb.check_completed_generation != smb.check_requested_generation;
}

void ExportCoordinator::record_request(
    const ExportRequest &request, const ExportEndpointStatus &status) {
    auto &state = schedule(request.endpoint);
    const bool full_sync = request.operation != ExportOperation::Check &&
                           request.reason != ExportReason::Maintenance &&
                           !request.day[0];
    if (request.operation == ExportOperation::Check ||
        (request.endpoint == ExportEndpoint::Smb && full_sync)) {
        if (state.check_completed_generation != status.config_generation) {
            state.check_requested_generation = status.config_generation;
            state.request_completed_sequence = status.completed_sequence;
        }
    }
    if (full_sync) state.backfill_queued_generation = status.config_generation;
}

bool ExportCoordinator::request_external(const ExportRequest &request) {
    if (!task_) return false;

    const auto current = control_snapshot();
    if (!external_request_allowed(current) || !task_->request(request)) {
        return false;
    }

    // A manual SleepHQ check did not participate in startup bookkeeping.
    if (request.operation != ExportOperation::Check) {
        record_request(request, current.endpoint(request.endpoint));
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
    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        if (!current.endpoint(endpoint).configured) continue;

        if (!queued) {
            if (!request_external({endpoint, ExportOperation::Sync,
                                   ExportReason::Manual})) {
                return false;
            }
            queued = true;
        } else {
            schedule(endpoint).manual_pending = true;
        }
    }
    return queued;
}

bool ExportCoordinator::request_smb_sync() {
    return request_external({ExportEndpoint::Smb, ExportOperation::Sync,
                             ExportReason::Manual});
}

bool ExportCoordinator::request_smb_verify() {
    return request_external({ExportEndpoint::Smb, ExportOperation::Reconcile,
                             ExportReason::Manual});
}

bool ExportCoordinator::request_sleephq_sync() {
    return request_external({ExportEndpoint::SleepHq, ExportOperation::Sync,
                             ExportReason::Manual});
}

bool ExportCoordinator::request_sleephq_sync_day(const char *day) {
    if (!day || !storage_export_is_datalog_day_name(day)) return false;

    ExportRequest request{ExportEndpoint::SleepHq, ExportOperation::Sync,
                          ExportReason::Manual};
    memcpy(request.day, day, sizeof(request.day));
    return request_external(request);
}

bool ExportCoordinator::request_sleephq_check() {
    return request_external({ExportEndpoint::SleepHq, ExportOperation::Check,
                             ExportReason::Manual});
}

void ExportCoordinator::poll(const ExportReportActivity &report,
                             const ActivitySnapshot &activity,
                             uint32_t now_ms) {
    if (!task_) return;

    const auto status = task_->control_snapshot();
    status_snapshot_ = status;
    observe_startup_check(ExportEndpoint::Smb, status.endpoint(ExportEndpoint::Smb));

    poll_post_therapy(report, activity.realtime_stream_active,
                      activity.therapy_active, status, now_ms);
    poll_manual_sync(status);
    maybe_preempt_full_reconcile(report, activity, status);

    if (status.endpoint(ExportEndpoint::SleepHq).state == ExportState::Working) {
        task_->defer_smb_until(due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS));
    }

    const bool startup_idle =
        startup_idle_work_allowed(now_ms) && !report.foreground_active &&
        !activity.therapy_active && !activity.realtime_stream_active &&
        !activity.ota_install_active;

    if (startup_idle) queue_startup_checks(status);
    poll_startup_backfill(report, activity, status, now_ms);
    poll_full_reconcile(report, activity, status, now_ms);
}

void ExportCoordinator::poll_manual_sync(
    const ExportTaskControlSnapshot &status) {
    if (status.command_pending || status.busy) return;

    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        auto &state = schedule(endpoint);
        if (!state.manual_pending) continue;

        if (request_external({endpoint, ExportOperation::Sync, ExportReason::Manual})) {
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
    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        auto &state = schedule(endpoint);
        state.post_therapy_pending = status.endpoint(endpoint).configured;
        state.post_therapy_due_ms = 0;
        pending = pending || state.post_therapy_pending;
    }

    post_therapy_.report_settle_due_ms =
        pending ? due_after(now_ms, AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS) : 0;
    post_therapy_.report_settle_deadline_ms =
        pending ? due_after(now_ms, AC_EXPORT_POST_THERAPY_SETTLE_MAX_WAIT_MS) : 0;

    if (post_therapy_.report_settle_due_ms != 0) {
        task_->defer_smb_until(post_therapy_.report_settle_due_ms);
    }
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
    const bool deadline_reached =
        post_therapy_.report_settle_deadline_ms != 0 &&
        static_cast<int32_t>(now_ms -
                             post_therapy_.report_settle_deadline_ms) >= 0;
    if (stream_activity_active ||
        (report.post_therapy_settle_pending &&
         !deadline_reached)) {
        if (task_) {
            task_->defer_smb_until(
                due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS));
        }
        return;
    }

    if (report.post_therapy_settle_pending) {
        Log::logf(CAT_EXPORT, LOG_WARN,
                  "post-therapy exports proceeding after report wait\n");
    }

    post_therapy_.report_settle_due_ms = 0;
    post_therapy_.report_settle_deadline_ms = 0;
    schedule(ExportEndpoint::Smb).post_therapy_due_ms = 0;
}

void ExportCoordinator::queue_post_therapy(
    bool stream_activity_active, const ExportTaskControlSnapshot &status,
    uint32_t now_ms) {
    if (post_therapy_.report_settle_due_ms != 0) return;

    bool previous_active = false;
    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        auto &state = schedule(endpoint);
        if (state.post_therapy_pending) {
            const bool smb = endpoint == ExportEndpoint::Smb;
            const bool due_in_future = state.post_therapy_due_ms != 0 &&
                static_cast<int32_t>(now_ms - state.post_therapy_due_ms) < 0;

            // SMB's idle deferral is already armed until its due time.
            if (smb && due_in_future) {
                previous_active = true;
                continue;
            }

            if (stream_activity_active || previous_active) {
                state.post_therapy_due_ms = 0;
                if (smb) {
                    task_->defer_smb_until(due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS));
                }
            } else if (state.post_therapy_due_ms == 0) {
                state.post_therapy_due_ms = due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
                if (smb) task_->defer_smb_until(state.post_therapy_due_ms);
            } else if (!due_in_future) {
                const auto current = control_snapshot();
                const ExportRequest request{
                    endpoint, ExportOperation::Sync, ExportReason::PostTherapy,
                };
                if (task_->request(request)) {
                    record_request(request, current.endpoint(endpoint));
                    state.post_therapy_pending = false;
                    state.post_therapy_due_ms = 0;
                } else {
                    state.post_therapy_due_ms = due_after(now_ms, AC_EXPORT_TASK_BUSY_RECHECK_MS);
                }
            }
        }

        previous_active = previous_active || state.post_therapy_pending ||
                          status.endpoint(endpoint).active();
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
    if (static_cast<int32_t>(now_ms - AC_RUNTIME_STARTUP_IDLE_GRACE_MS) < 0) {
        return false;
    }

    startup_idle_grace_complete_ = true;
    return true;
}

void ExportCoordinator::observe_startup_check(
    ExportEndpoint endpoint, const ExportEndpointStatus &status) {
    auto &state = schedule(endpoint);
    if (!status.configured || status.config_generation == 0) {
        state.check_requested_generation = 0;
        state.check_completed_generation = 0;
        state.request_completed_sequence = 0;
        return;
    }

    // SleepHQ explicitly confirms account resolution. SMB confirms a run;
    // a terminal check failure also lets the next endpoint attempt its check.
    if (endpoint == ExportEndpoint::SleepHq) {
        if (status.completed_check_generation == status.config_generation) {
            state.check_completed_generation = status.config_generation;
        }
        return;
    }

    if (state.check_requested_generation != status.config_generation) {
        state.check_completed_generation = 0;
        return;
    }
    if ((status.state == ExportState::Error && !status.pending) ||
        (status.ready() && status.completed_sequence != state.request_completed_sequence)) {
        state.check_completed_generation = status.config_generation;
    }
}

void ExportCoordinator::queue_startup_checks(
    const ExportTaskControlSnapshot &status) {
    if (!status.network_ready) return;

    bool previous_settled = true;
    bool previous_active = false;
    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        const auto &current = status.endpoint(endpoint);
        auto &state = schedule(endpoint);
        if (!previous_settled || previous_active) return;

        observe_startup_check(endpoint, current);
        if (current.configured && current.config_generation != 0 &&
            state.check_completed_generation != current.config_generation &&
            state.check_requested_generation != current.config_generation) {
            bool busy = current.awaiting() || current.state == ExportState::Working;
            // A pending endpoint must finish before an earlier startup check.
            for (const auto &other : status.endpoints) {
                busy = busy || other.pending || other.state == ExportState::Working;
            }

            if (!busy) {
                const ExportRequest request{
                    endpoint, ExportOperation::Check, ExportReason::Startup,
                };
                if (task_->request(request)) record_request(request, current);
            }
        }

        previous_settled = !current.configured ||
            state.check_completed_generation == current.config_generation;
        previous_active = current.active();
    }
}

void ExportCoordinator::poll_startup_backfill(
    const ExportReportActivity &report, const ActivitySnapshot &activity,
    const ExportTaskControlSnapshot &status, uint32_t now_ms) {
    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        const auto &current = status.endpoint(endpoint);
        auto &state = schedule(endpoint);
        if (!current.configured) {
            state.backfill_queued_generation = 0;
            continue;
        }

        const bool check_complete =
            state.check_completed_generation == current.config_generation;
        const bool failed_account_check =
            endpoint == ExportEndpoint::SleepHq &&
            state.check_requested_generation == current.config_generation &&
            !current.active() && !status.command_pending;

        if (!check_complete && !failed_account_check) {
            startup_backfill_due_ms_ = 0;
            return;
        }
    }

    bool busy = !status.network_ready || task_->endpoint_work_claimed() ||
                status.command_pending || report.foreground_active ||
                report.background_active || activity.therapy_active ||
                activity.realtime_stream_active || activity.ota_install_active;
    for (const auto &endpoint : status.endpoints) busy = busy || endpoint.active();

    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        const auto &current = status.endpoint(endpoint);
        auto &state = schedule(endpoint);
        const bool ready = current.configured && current.ready() &&
            state.check_completed_generation == current.config_generation &&
            state.backfill_queued_generation != current.config_generation;
        if (!ready || busy) continue;

        if (startup_backfill_due_ms_ == 0) {
            startup_backfill_due_ms_ = due_after(now_ms, AC_EXPORT_ACTIVITY_GRACE_MS);
            return;
        }
        if (static_cast<int32_t>(now_ms - startup_backfill_due_ms_) < 0) return;

        const ExportRequest request{
            endpoint, ExportOperation::Sync, ExportReason::IdleBackfill,
        };
        if (task_->request(request)) {
            record_request(request, current);
            startup_backfill_due_ms_ = 0;
        }
        return;
    }
    startup_backfill_due_ms_ = 0;
}

bool ExportCoordinator::startup_backfill_pending(
    const ExportTaskControlSnapshot &status) const {
    for (const auto endpoint : AC_EXPORT_ENDPOINT_ORDER) {
        const auto &current = status.endpoint(endpoint);
        const auto &state = schedule(endpoint);
        if (current.configured &&
            state.check_completed_generation == current.config_generation &&
            state.backfill_queued_generation != current.config_generation) {
            return true;
        }
    }
    return false;
}

void ExportCoordinator::maybe_preempt_full_reconcile(
    const ExportReportActivity &report,
    const ActivitySnapshot &activity,
    const ExportTaskControlSnapshot &task_status) {
    const auto &smb = task_status.endpoint(ExportEndpoint::Smb);
    if (!task_ || !smb.scheduled_reconcile) return;

    const bool smb_startup_pending =
        smb.configured &&
        schedule(ExportEndpoint::Smb).check_completed_generation !=
            smb.config_generation;
    const bool post_pending = post_therapy_pending();
    const bool priority_work =
        !task_status.network_ready || report.foreground_active ||
        report.background_active || activity.therapy_active ||
        activity.realtime_stream_active || activity.ota_install_active ||
        smb_startup_pending || post_pending ||
        startup_backfill_pending(task_status) ||
        task_status.endpoint(ExportEndpoint::SleepHq).active();
    if (!priority_work) return;

    full_reconcile_.idle_due_ms = 0;
    task_->cancel_smb_scheduled_reconcile();
}

bool ExportCoordinator::full_reconcile_prerequisites_complete(
    const ExportTaskControlSnapshot &task_status) const {
    const auto &smb = task_status.endpoint(ExportEndpoint::Smb);
    const auto &sleephq = task_status.endpoint(ExportEndpoint::SleepHq);
    if (!task_status.network_ready || task_status.command_pending ||
        !smb.configured || !smb.ready() || sleephq.active()) {
        return false;
    }
    if (schedule(ExportEndpoint::Smb).check_completed_generation !=
        smb.config_generation) {
        return false;
    }
    if (post_therapy_pending() || startup_backfill_pending(task_status)) {
        return false;
    }

    if (!sleephq.configured) return true;
    const bool check_succeeded =
        schedule(ExportEndpoint::SleepHq).check_completed_generation ==
        sleephq.config_generation;
    const bool check_attempted =
        schedule(ExportEndpoint::SleepHq).check_requested_generation ==
        sleephq.config_generation;
    if (!check_succeeded && !check_attempted) {
        return false;
    }
    if (sleephq.awaiting() || sleephq.state == ExportState::Working) {
        return false;
    }
    if (check_succeeded &&
        schedule(ExportEndpoint::SleepHq).backfill_queued_generation !=
            sleephq.config_generation) {
        return false;
    }
    return true;
}

void ExportCoordinator::poll_full_reconcile(
    const ExportReportActivity &report,
    const ActivitySnapshot &activity,
    const ExportTaskControlSnapshot &task_status,
    uint32_t now_ms) {
    if (!task_) return;

    const auto &smb = task_status.endpoint(ExportEndpoint::Smb);
    if (full_reconcile_.config_generation !=
        smb.config_generation) {
        reset_full_reconcile();
        full_reconcile_.config_generation =
            smb.config_generation;
    }

    if (full_reconcile_.queued) {
        if (smb.scheduled_reconcile) {
            return;
        }
        if (task_status.command_pending || smb.active()) {
            return;
        }

        reset_full_reconcile();
        full_reconcile_.config_generation =
            smb.config_generation;
    }

    const uint64_t now_epoch = storage_export_current_epoch_seconds_or_zero();
    const uint64_t last_reconcile = smb.last_reconcile_epoch;
    const bool due =
        now_epoch != 0 &&
        (last_reconcile == 0 ||
         now_epoch >= last_reconcile +
             AC_EXPORT_FULL_RECONCILE_INTERVAL_SECONDS);
    const bool runtime_idle =
        !report.foreground_active && !report.background_active &&
        !activity.therapy_active && !activity.realtime_stream_active &&
        !activity.ota_install_active;
    if (!due || !runtime_idle ||
        !full_reconcile_prerequisites_complete(task_status)) {
        full_reconcile_.idle_due_ms = 0;
        return;
    }

    if (full_reconcile_.idle_due_ms == 0) {
        full_reconcile_.idle_due_ms = due_after(
            now_ms, AC_EXPORT_FULL_RECONCILE_IDLE_GRACE_MS);
        return;
    }
    if (static_cast<int32_t>(now_ms - full_reconcile_.idle_due_ms) < 0) {
        return;
    }

    if (task_->request({ExportEndpoint::Smb, ExportOperation::Reconcile,
                        ExportReason::Maintenance})) {
        full_reconcile_.queued = true;
        full_reconcile_.idle_due_ms = 0;
    }
}

void ExportCoordinator::reset_full_reconcile() {
    full_reconcile_ = FullReconcileState();
}

}  // namespace aircannect
