#pragma once

#include <stddef.h>
#include <stdint.h>
#include <array>

namespace aircannect {

enum class ExportEndpoint : uint8_t { Smb, SleepHq };
// Runtime capacity only; endpoint slots are never persisted.
constexpr size_t AC_EXPORT_ENDPOINT_CAPACITY = 8;

struct ExportEndpointOrder {
    std::array<ExportEndpoint, AC_EXPORT_ENDPOINT_CAPACITY> ids = {};
    size_t count = 0;

    const ExportEndpoint *begin() const { return ids.data(); }
    const ExportEndpoint *end() const { return ids.data() + count; }
};

enum class ExportOperation : uint8_t { Check, Sync, Reconcile };
enum class ExportReason : uint8_t {
    Manual, Startup, IdleBackfill, PostTherapy, Maintenance,
};
enum class ExportState : uint8_t { Disabled, Idle, Pending, Working, Error };

enum class ExportPriority : uint8_t { Maintenance, Normal };

struct ExportWorkPolicy {
    ExportPriority priority = ExportPriority::Normal;
    bool preemptible = false;
    bool exclusive = true;
};

struct ExportOperationRule {
    ExportOperation operation;
    ExportReason reason;
    bool accepts_day = false;
    ExportWorkPolicy work = {};
};

struct ExportBackendDefinition {
    ExportEndpoint id;
    const ExportOperationRule *rules;
    size_t rule_count;
    bool sync_confirms_readiness = false;
    uint32_t maintenance_interval_seconds = 0;

    const ExportOperationRule *rule(ExportOperation operation,
                                    ExportReason reason) const {
        for (size_t i = 0; i < rule_count; ++i) {
            if (rules[i].operation == operation && rules[i].reason == reason) {
                return &rules[i];
            }
        }
        return nullptr;
    }

    bool requires_check() const {
        return rule(ExportOperation::Check, ExportReason::Startup) != nullptr;
    }
};

struct ExportRequest {
    ExportEndpoint endpoint = ExportEndpoint::Smb;
    ExportOperation operation = ExportOperation::Sync;
    ExportReason reason = ExportReason::Manual;
    char day[9] = {};

    bool supported(const ExportBackendDefinition &definition) const {
        const auto *rule = definition.rule(operation, reason);
        return endpoint == definition.id && rule && (!day[0] || rule->accepts_day);
    }
};

// Scheduling facts only. Protocol progress and errors stay in engine status.
struct ExportEndpointStatus {
    const ExportBackendDefinition *definition = nullptr;
    ExportState state = ExportState::Disabled;
    bool configured = false;
    bool network_available = false;
    bool pending = false;
    bool durable_state_pending = false;
    uint32_t config_generation = 0;
    uint32_t ready_generation = 0;

    ExportWorkPolicy work;
    bool maintenance_pending = false;
    uint64_t last_maintenance_epoch = 0;

    bool check_succeeded() const {
        return config_generation != 0 && ready_generation == config_generation;
    }

    bool backfill_ready() const {
        return definition && (!definition->requires_check() || check_succeeded());
    }

    bool can_preempt(ExportWorkPolicy incoming) const {
        return !durable_state_pending && work.preemptible &&
               incoming.priority > work.priority;
    }

    bool executing() const {
        return durable_state_pending || state == ExportState::Working;
    }

    bool awaiting() const { return pending || state == ExportState::Pending; }
    bool active() const { return executing() || (awaiting() && network_available); }
    bool busy() const { return executing() || pending; }
    bool ready() const { return state == ExportState::Idle && !pending; }
};

}  // namespace aircannect
