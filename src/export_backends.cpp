#include "export_backends.h"

#include "board_report.h"
#include "sleephq_sync_engine.h"
#include "storage_sync_engine.h"

namespace aircannect {

namespace {

constexpr ExportOperationRule SMB_RULES[] = {
    {ExportOperation::Check, ExportReason::Startup},
    {ExportOperation::Sync, ExportReason::Manual},
    {ExportOperation::Sync, ExportReason::IdleBackfill},
    {ExportOperation::Sync, ExportReason::PostTherapy},
    {ExportOperation::Reconcile, ExportReason::Manual},
    {ExportOperation::Reconcile, ExportReason::Maintenance, false,
     {ExportPriority::Maintenance, true, true}},
};

constexpr ExportOperationRule SLEEPHQ_RULES[] = {
    {ExportOperation::Check, ExportReason::Startup},
    {ExportOperation::Check, ExportReason::Manual},
    {ExportOperation::Sync, ExportReason::Manual, true},
    {ExportOperation::Sync, ExportReason::IdleBackfill},
    {ExportOperation::Sync, ExportReason::PostTherapy},
};

bool request_engine(StorageSyncEngine &engine, const ExportRequest &request) {
    switch (request.operation) {
        case ExportOperation::Check:
            return engine.request_startup_check();
        case ExportOperation::Reconcile:
            return request.reason == ExportReason::Maintenance
                ? engine.request_scheduled_reconcile()
                : engine.request_manual_reconcile();
        case ExportOperation::Sync:
            if (request.reason == ExportReason::PostTherapy) {
                return engine.request_post_therapy_sync();
            }
            if (request.reason == ExportReason::IdleBackfill) {
                return engine.request_idle_backfill();
            }
            return engine.request_manual_sync();
    }
    return false;
}

bool request_engine(SleepHqSyncEngine &engine, const ExportRequest &request) {
    if (request.operation == ExportOperation::Check) {
        return engine.request_check(request.reason == ExportReason::Startup
                                        ? "startup_check" : "manual");
    }
    if (request.reason == ExportReason::PostTherapy) {
        return engine.request_post_therapy_sync();
    }
    if (request.day[0]) return engine.request_sync_day(request.day, "manual_day");

    return engine.request_sync(request.reason == ExportReason::IdleBackfill
                                   ? "idle_backfill" : "manual");
}

void cancel_engine(StorageSyncEngine &engine, ExportReason reason) {
    if (reason == ExportReason::PostTherapy) engine.cancel_post_therapy_sync();
    if (reason == ExportReason::Maintenance) engine.cancel_scheduled_reconcile();
}

void cancel_engine(SleepHqSyncEngine &engine, ExportReason reason) {
    if (reason == ExportReason::PostTherapy) engine.cancel_post_therapy_sync();
}

template <typename Status>
ExportEndpointStatus common_status(const Status &source) {
    ExportEndpointStatus out;
    out.state = source.state;
    out.configured = source.configured;
    out.network_available = source.network_available;
    out.pending = source.pending;
    out.config_generation = source.config_generation;
    return out;
}

ExportEndpointStatus engine_status(StorageSyncEngine &engine) {
    const auto source = engine.runtime_status();
    auto out = common_status(source);
    out.configured = source.enabled && source.configured;
    out.durable_state_pending = source.durable_state_pending;
    out.ready_generation = source.completed_sequence ? source.config_generation : 0;
    out.maintenance_pending = source.scheduled_reconcile;
    out.last_maintenance_epoch = source.last_reconcile_epoch;
    if (source.scheduled_reconcile) {
        out.work = SMB_EXPORT_DEFINITION.rule(
            ExportOperation::Reconcile, ExportReason::Maintenance)->work;
    }
    return out;
}

ExportEndpointStatus engine_status(SleepHqSyncEngine &engine) {
    const auto source = engine.runtime_status();
    auto out = common_status(source);
    out.ready_generation = source.completed_check_generation;
    return out;
}

void defer_engine(StorageSyncEngine &engine, uint32_t until_ms) {
    engine.defer_idle_work_until(until_ms);
}

void defer_engine(SleepHqSyncEngine &, uint32_t) {}

template <typename Engine>
const ExportBackend::Operations &operations() {
    static const ExportBackend::Operations ops = {
        [](void *e, const ExportRequest &r) {
            return request_engine(*static_cast<Engine *>(e), r);
        },
        [](void *e) { return static_cast<Engine *>(e)->step(); },
        [](void *e) { return engine_status(*static_cast<Engine *>(e)); },
        [](void *e) { return static_cast<Engine *>(e)->release_media(); },
        [](void *e, ExportReason r) {
            cancel_engine(*static_cast<Engine *>(e), r);
        },
        [](void *e, bool v) {
            static_cast<Engine *>(e)->set_network_available(v);
        },
        [](void *e, bool v) {
            static_cast<Engine *>(e)->set_runtime_blocked(v);
        },
        [](void *e, uint32_t until_ms) {
            defer_engine(*static_cast<Engine *>(e), until_ms);
        },
    };
    return ops;
}

}  // namespace

const ExportBackendDefinition SMB_EXPORT_DEFINITION = {
    ExportEndpoint::Smb, SMB_RULES, sizeof(SMB_RULES) / sizeof(SMB_RULES[0]),
    true, AC_EXPORT_FULL_RECONCILE_INTERVAL_SECONDS,
};

const ExportBackendDefinition SLEEPHQ_EXPORT_DEFINITION = {
    ExportEndpoint::SleepHq, SLEEPHQ_RULES,
    sizeof(SLEEPHQ_RULES) / sizeof(SLEEPHQ_RULES[0]),
};

ExportBackend make_export_backend(StorageSyncEngine &engine) {
    return {&engine, SMB_EXPORT_DEFINITION, operations<StorageSyncEngine>()};
}

ExportBackend make_export_backend(SleepHqSyncEngine &engine) {
    return {&engine, SLEEPHQ_EXPORT_DEFINITION, operations<SleepHqSyncEngine>()};
}

}  // namespace aircannect
