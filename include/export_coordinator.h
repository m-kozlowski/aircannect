#pragma once

#include <stdint.h>

#include "export_task.h"
#include "runtime_snapshots.h"

namespace aircannect {

struct ExportReportActivity {
    bool foreground_active = false;
    bool background_active = false;
    bool post_therapy_settle_pending = false;
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
    ExportTaskControlSnapshot status_snapshot() const {
        return status_snapshot_;
    }
    StorageSyncStatus smb_status() const;
    SleepHqSyncStatus sleephq_status() const;
    ExportStatusSnapshot endpoint_status_snapshot() const;
    ExportSmbStatusSnapshot smb_snapshot() const;
    ExportSleepHqStatusSnapshot sleephq_snapshot() const;

    // external requests
    bool request(const ExportRequest &request);
    bool request_sync();

private:
    struct PostTherapyState {
        bool state_initialized = false;
        bool last_therapy_active = false;
        bool cancel_pending = false;
        uint32_t report_settle_due_ms = 0;
        uint32_t report_settle_deadline_ms = 0;
    };

    struct EndpointSchedule {
        uint32_t check_requested_generation = 0;
        uint32_t backfill_queued_generation = 0;
        bool post_therapy_pending = false;
        uint32_t post_therapy_due_ms = 0;
        bool manual_pending = false;
        uint32_t maintenance_generation = 0;
        uint32_t maintenance_due_ms = 0;
        bool maintenance_queued = false;
    };

    // task snapshots and requests
    static uint32_t due_after(uint32_t now_ms, uint32_t delay_ms);
    ExportTaskControlSnapshot control_snapshot() const;
    EndpointSchedule &schedule(ExportEndpoint id);
    const EndpointSchedule &schedule(ExportEndpoint id) const;
    void record_request(const ExportRequest &request,
                        const ExportEndpointStatus &status);

    // post-therapy sequence
    void poll_post_therapy(const ExportReportActivity &report,
                           bool stream_activity_active,
                           bool therapy_active,
                           const ExportTaskControlSnapshot &status,
                           uint32_t now_ms);
    void reset_post_therapy_after_running();
    void arm_post_therapy_after_stop(uint32_t now_ms);
    void maybe_finish_report_settle(const ExportReportActivity &report,
                                    bool stream_activity_active,
                                    uint32_t now_ms);
    void queue_post_therapy(bool stream_activity_active,
                            const ExportTaskControlSnapshot &status,
                            uint32_t now_ms);
    bool post_therapy_pending() const;
    void defer_idle_work(const ExportTaskControlSnapshot &status,
                         uint32_t until_ms);

    // startup and idle backfill
    bool startup_idle_work_allowed(uint32_t now_ms);
    bool check_settled(ExportEndpoint endpoint,
                       const ExportTaskControlSnapshot &status) const;
    void queue_startup_checks(const ExportTaskControlSnapshot &status);
    void poll_startup_backfill(
        const ExportReportActivity &report,
        const ActivitySnapshot &activity,
        const ExportTaskControlSnapshot &task_status,
        uint32_t now_ms);
    bool startup_backfill_pending(
        const ExportTaskControlSnapshot &task_status) const;

    // manual all-endpoint sync
    void poll_manual_sync(const ExportTaskControlSnapshot &task_status);

    // preemptible endpoint maintenance
    void poll_maintenance(const ExportReportActivity &report,
                          const ActivitySnapshot &activity,
                          const ExportTaskControlSnapshot &status,
                          uint32_t now_ms);

    // export task command/status port
    ExportTask *task_ = nullptr;

    // coordinator state
    PostTherapyState post_therapy_;
    std::array<EndpointSchedule, AC_EXPORT_ENDPOINT_CAPACITY> endpoints_;
    bool startup_idle_grace_complete_ = false;
    uint32_t startup_backfill_due_ms_ = 0;
    ExportTaskControlSnapshot status_snapshot_;
};

}  // namespace aircannect
