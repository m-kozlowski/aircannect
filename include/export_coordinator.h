#pragma once

#include <stdint.h>

#include "export_task.h"
#include "runtime_snapshots.h"

namespace aircannect {

struct ExportReportActivity {
    bool foreground_active = false;
    bool background_active = false;
};

// Owns policy that spans export endpoints. The endpoint engines own their
// protocols and durable state; this coordinator owns when they may run and in
// what order.
class ExportCoordinator {
public:
    // lifecycle
    void begin(ExportTask &task);

    // scheduling policy
    void poll(const ExportReportActivity &report,
              const ActivitySnapshot &activity,
              uint32_t now_ms);
    bool endpoint_work_claimed() const;
    StorageSyncStatus smb_status() const;
    SleepHqSyncStatus sleephq_status() const;
    ExportSmbStatusSnapshot smb_snapshot() const;
    ExportSleepHqStatusSnapshot sleephq_snapshot() const;

    // external requests
    bool request_smb_sync();
    bool request_smb_verify();
    bool request_sleephq_sync();
    bool request_sleephq_sync_day(const char *day);
    bool request_sleephq_check();

private:
    struct PostTherapyState {
        bool state_initialized = false;
        bool last_therapy_active = false;
        uint32_t report_settle_due_ms = 0;
        bool storage_pending = false;
        bool storage_grace_armed = false;
        bool storage_fallback_reported = false;
        uint32_t storage_due_ms = 0;
        uint32_t storage_deadline_ms = 0;
        bool sleephq_pending = false;
        bool sleephq_grace_armed = false;
        uint32_t sleephq_due_ms = 0;
        uint32_t sleephq_deadline_ms = 0;
    };

    struct StartupCheckState {
        uint32_t smb_requested_generation = 0;
        uint32_t smb_completed_generation = 0;
        uint32_t smb_request_completed_sequence = 0;
        uint32_t sleephq_requested_generation = 0;
        uint32_t sleephq_completed_generation = 0;
        bool idle_grace_complete = false;
    };

    struct IdleBackfillState {
        uint32_t queued_generation = 0;
        uint32_t armed_generation = 0;
        bool pending = false;
        uint32_t due_ms = 0;
    };

    struct FullReconcileState {
        uint32_t config_generation = 0;
        uint32_t idle_due_ms = 0;
        bool queued = false;
    };

    static uint32_t due_after(uint32_t now_ms, uint32_t delay_ms);
    static bool external_request_allowed(
        const ExportTaskControlSnapshot &status);
    ExportTaskControlSnapshot control_snapshot() const;

    // post-therapy sequence
    void poll_post_therapy(const ExportReportActivity &report,
                           bool stream_activity_active,
                           bool therapy_active,
                           bool storage_sync_active,
                           uint32_t now_ms);
    void reset_post_therapy_after_running();
    void arm_post_therapy_after_stop(uint32_t now_ms);
    void maybe_finish_report_settle(const ExportReportActivity &report,
                                    bool stream_activity_active,
                                    uint32_t now_ms);
    void maybe_queue_storage_sync(const ExportReportActivity &report,
                                  bool stream_activity_active,
                                  uint32_t now_ms);
    void maybe_queue_post_therapy_sleephq(
        const ExportReportActivity &report,
        bool stream_activity_active,
        bool storage_sync_active,
        uint32_t now_ms);
    void queue_post_therapy_storage_sync(uint32_t now_ms);
    void clear_post_therapy_sleephq();

    // startup and idle backfill
    bool startup_idle_work_allowed(uint32_t now_ms);
    void observe_smb_startup_check(StorageSyncRuntimeStatus status);
    void maybe_queue_smb_startup_check(
        bool network_connected,
        StorageSyncRuntimeStatus status,
        SleepHqSyncRuntimeStatus sleephq);
    void maybe_queue_sleephq_startup_check(bool network_connected,
                                           bool smb_startup_complete,
                                           bool storage_sync_active,
                                           SleepHqSyncRuntimeStatus status);
    void poll_sleephq_idle_backfill(const ExportReportActivity &report,
                                    bool network_connected,
                                    bool stream_activity_active,
                                    bool therapy_active,
                                    bool storage_sync_active,
                                    SleepHqSyncRuntimeStatus status,
                                    uint32_t now_ms);
    void clear_idle_backfill();

    // full SMB reconcile
    void maybe_preempt_full_reconcile(
        const ExportReportActivity &report,
        const ActivitySnapshot &activity,
        const ExportTaskControlSnapshot &task_status);
    void poll_full_reconcile(const ExportReportActivity &report,
                             const ActivitySnapshot &activity,
                             const ExportTaskControlSnapshot &task_status,
                             uint32_t now_ms);
    bool full_reconcile_prerequisites_complete(
        const ExportTaskControlSnapshot &task_status) const;
    void reset_full_reconcile();

    // export task command/status port
    ExportTask *task_ = nullptr;

    // coordinator state
    PostTherapyState post_therapy_;
    StartupCheckState startup_check_;
    IdleBackfillState idle_backfill_;
    FullReconcileState full_reconcile_;
};

}  // namespace aircannect
