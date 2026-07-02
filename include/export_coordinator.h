#pragma once

#include <stdint.h>

#include "app_config.h"
#include "report_manager.h"
#include "rpc_arbiter.h"
#include "sleephq_sync_job.h"
#include "storage_sync_job.h"

namespace aircannect {

// Owns policy that spans export endpoints. The endpoint jobs own their
// protocols and durable state; this coordinator owns when they may run and in
// what order.
class ExportCoordinator {
public:
    // lifecycle
    void begin(StorageSyncJob *storage_sync,
               SleepHqSyncJob *sleephq_sync);

    // scheduling policy
    void poll(RpcArbiter &arbiter,
              ReportManager &report,
              const AppConfigData &config,
              bool network_connected,
              bool resmed_ota_active,
              bool esp_ota_active,
              uint32_t now_ms);

    // external requests
    bool request_smb_sync();
    bool request_smb_verify();
    bool request_sleephq_sync();
    bool request_sleephq_sync_day(const char *day);
    bool request_sleephq_check();

private:
    struct PostTherapyState {
        As11TherapyState last_state = As11TherapyState::Unknown;
        uint32_t summary_refresh_due_ms = 0;
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

    // shared gates
    bool export_network_ready(bool network_connected, uint32_t now_ms);

    // post-therapy sequence
    void poll_post_therapy(RpcArbiter &arbiter,
                           ReportManager &report,
                           bool storage_sync_active,
                           uint32_t now_ms);
    void reset_post_therapy_after_running();
    void arm_post_therapy_after_stop(uint32_t now_ms);
    void maybe_refresh_summary(RpcArbiter &arbiter,
                               ReportManager &report,
                               uint32_t now_ms);
    void maybe_queue_storage_sync(RpcArbiter &arbiter,
                                  ReportManager &report,
                                  uint32_t now_ms);
    void maybe_queue_post_therapy_sleephq(RpcArbiter &arbiter,
                                          ReportManager &report,
                                          bool storage_sync_active,
                                          uint32_t now_ms);
    void queue_post_therapy_storage_sync(uint32_t now_ms);
    void clear_post_therapy_sleephq();

    // startup and idle backfill
    void maybe_queue_sleephq_startup_check(bool network_connected,
                                           bool storage_sync_active,
                                           SleepHqSyncRuntimeStatus status);
    void poll_sleephq_idle_backfill(RpcArbiter &arbiter,
                                    ReportManager &report,
                                    bool network_connected,
                                    bool storage_sync_active,
                                    SleepHqSyncRuntimeStatus status,
                                    uint32_t now_ms);
    void clear_idle_backfill();

    // endpoint jobs
    StorageSyncJob *storage_sync_ = nullptr;
    SleepHqSyncJob *sleephq_sync_ = nullptr;

    // coordinator state
    PostTherapyState post_therapy_;
    StartupCheckState startup_check_;
    IdleBackfillState idle_backfill_;
    uint32_t network_connected_since_ms_ = 0;
};

}  // namespace aircannect
