#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class ExportEndpoint : uint8_t { Smb, SleepHq, Count };
constexpr size_t AC_EXPORT_ENDPOINT_COUNT =
    static_cast<size_t>(ExportEndpoint::Count);
constexpr ExportEndpoint AC_EXPORT_ENDPOINT_ORDER[] = {
    ExportEndpoint::Smb, ExportEndpoint::SleepHq,
};

enum class ExportOperation : uint8_t { Check, Sync, Reconcile };
enum class ExportReason : uint8_t {
    Manual, Startup, IdleBackfill, PostTherapy, Maintenance,
};
enum class ExportState : uint8_t { Disabled, Idle, Pending, Working, Error };

struct ExportRequest {
    ExportEndpoint endpoint = ExportEndpoint::Smb;
    ExportOperation operation = ExportOperation::Sync;
    ExportReason reason = ExportReason::Manual;
    char day[9] = {};

    bool supported() const;
};

// Scheduling facts only. Protocol progress and errors stay in engine status.
struct ExportEndpointStatus {
    ExportState state = ExportState::Disabled;
    bool configured = false;
    bool network_available = false;
    bool pending = false;
    bool durable_state_pending = false;
    uint32_t config_generation = 0;
    uint32_t completed_sequence = 0;
    uint32_t completed_check_generation = 0;

    // SMB maintenance; unused by endpoints without reconcile support.
    bool scheduled_reconcile = false;
    uint64_t last_reconcile_epoch = 0;

    bool executing() const {
        return durable_state_pending || state == ExportState::Working;
    }

    bool awaiting() const { return pending || state == ExportState::Pending; }
    bool active() const { return executing() || (awaiting() && network_available); }
    bool busy() const { return executing() || pending; }
    bool ready() const { return state == ExportState::Idle && !pending; }
};

}  // namespace aircannect
