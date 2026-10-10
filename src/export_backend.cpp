#include "export_backend.h"

#include "sleephq_sync_engine.h"
#include "storage_sync_engine.h"

namespace aircannect {

bool ExportRequest::supported() const {
    if (endpoint != ExportEndpoint::Smb &&
        endpoint != ExportEndpoint::SleepHq) {
        return false;
    }

    if (day[0]) {
        return endpoint == ExportEndpoint::SleepHq &&
               operation == ExportOperation::Sync &&
               reason == ExportReason::Manual;
    }

    switch (operation) {
        case ExportOperation::Check:
            return reason == ExportReason::Startup ||
                   (endpoint == ExportEndpoint::SleepHq &&
                    reason == ExportReason::Manual);
        case ExportOperation::Sync:
            return reason == ExportReason::Manual ||
                   reason == ExportReason::IdleBackfill ||
                   reason == ExportReason::PostTherapy;
        case ExportOperation::Reconcile:
            return endpoint == ExportEndpoint::Smb &&
                   (reason == ExportReason::Manual ||
                    reason == ExportReason::Maintenance);
    }
    return false;
}

namespace {

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
    out.completed_sequence = source.completed_sequence;
    out.scheduled_reconcile = source.scheduled_reconcile;
    out.last_reconcile_epoch = source.last_reconcile_epoch;
    return out;
}

ExportEndpointStatus engine_status(SleepHqSyncEngine &engine) {
    const auto source = engine.runtime_status();
    auto out = common_status(source);
    out.completed_check_generation = source.completed_check_generation;
    return out;
}

}  // namespace

template <typename Engine>
const ExportBackend::Operations &ExportBackend::operations() {
    static const Operations ops = {
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
    };
    return ops;
}

ExportBackend::ExportBackend(StorageSyncEngine &engine)
    : engine_(&engine), ops_(&operations<StorageSyncEngine>()) {}

ExportBackend::ExportBackend(SleepHqSyncEngine &engine)
    : engine_(&engine), ops_(&operations<SleepHqSyncEngine>()) {}

}  // namespace aircannect
