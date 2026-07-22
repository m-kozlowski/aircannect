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
    bool endpoint_work_active() const;
    ExportTaskStatus status() const;
    StorageSyncStatus smb_status() const;
    SleepHqSyncStatus sleephq_status() const;

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
        uint32_t storage_due_ms = 0;
        uint32_t storage_deadline_ms = 0;
        bool sleephq_pending = false;
        bool sleephq_grace_armed = false;
        uint32_t sleephq_due_ms = 0;
        uint32_t sleephq_deadline_ms = 0;
    };

    struct StartupCheckState {
        uint32_t requested_generation = 0;
        uint32_t completed_generation = 0;
    };

    struct IdleBackfillState {
        uint32_t queued_generation = 0;
        uint32_t armed_generation = 0;
        bool pending = false;
        uint32_t due_ms = 0;
    };

    static uint32_t due_after(uint32_t now_ms, uint32_t delay_ms);

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
    void maybe_queue_sleephq_startup_check(bool network_connected,
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

    // export task command/status port
    ExportTask *task_ = nullptr;

    // coordinator state
    PostTherapyState post_therapy_;
    StartupCheckState startup_check_;
    IdleBackfillState idle_backfill_;
};

}  // namespace aircannect
