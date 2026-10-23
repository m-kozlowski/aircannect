#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <array>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "export_endpoint_config.h"
#include "runtime_snapshots.h"
#include "sleephq_sync_engine.h"
#include "storage_sync_engine.h"

namespace aircannect {

class StorageAtomicWritePort;
class StoragePathPort;
class StorageReadPort;
class StorageScanPort;
class StorageStreamPort;

struct ExportTaskControlSnapshot {
    bool initialized = false;
    bool task_started = false;
    bool network_ready = false;
    bool runtime_blocked = true;
    bool command_pending = false;
    bool active = false;
    bool busy = false;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;
    ExportEndpointOrder order;
    std::array<ExportEndpointStatus, AC_EXPORT_ENDPOINT_CAPACITY> endpoints;

    bool accepts(ExportWorkPolicy incoming) const {
        if (command_pending) return false;

        for (const auto id : order) {
            const auto &current = endpoint(id);
            if (!current.busy()) continue;
            if (current.can_preempt(incoming)) continue;
            if (current.durable_state_pending || incoming.exclusive ||
                current.work.exclusive) return false;
        }
        return true;
    }

    const ExportEndpointStatus &endpoint(ExportEndpoint id) const {
        return endpoints[static_cast<size_t>(id)];
    }

    ExportEndpointStatus &endpoint(ExportEndpoint id) {
        return endpoints[static_cast<size_t>(id)];
    }
#if AC_STACK_PROFILE_ENABLED
    uint32_t stack_high_water_words = 0;
#endif
};

struct ExportSmbStatusSnapshot {
    char smb_endpoint[AC_SMB_EXPORT_ENDPOINT_MAX] = {};
    StorageSyncStatus sync;
};

struct ExportSleepHqStatusSnapshot {
    char sleephq_team_id[AC_SLEEPHQ_ID_MAX] = {};
    char sleephq_device_id[AC_SLEEPHQ_ID_MAX] = {};
    SleepHqSyncStatus sync;
};

struct ExportStatusSnapshot {
    ExportSmbStatusSnapshot smb;
    ExportSleepHqStatusSnapshot sleephq;
};

// Sole task owner for the SMB and SleepHQ protocol engines. Public methods
// only publish immutable inputs, enqueue one typed command, or copy status.
class ExportTask {
public:
    ExportTask() = default;

    ExportTask(const ExportTask &) = delete;
    ExportTask &operator=(const ExportTask &) = delete;

    // lifecycle
    bool begin(const ExportEndpointConfig &config,
               StorageScanPort &scan_port,
               StorageReadPort &read_port,
               StorageStreamPort &stream_port,
               StorageAtomicWritePort &write_port,
               StoragePathPort &path_port);

    // immutable runtime inputs
    bool publish_config(const ExportEndpointConfig &config);
    void publish_activity(const ActivitySnapshot &activity);
    void publish_network(const NetworkSnapshot &network);
    void set_storage_suspended(bool suspended);
    bool storage_quiesced() const { return !task_ || storage_paused_.load(); }
    void defer_idle_until(ExportEndpoint endpoint, uint32_t until_ms);

    // endpoint commands
    bool request(const ExportRequest &request);
    void cancel_maintenance(ExportEndpoint endpoint);
    bool cancel_post_therapy();

    // Scheduled checks yield to exports; manual checks precede queued work.
    // Release on admission/start failure, otherwise after checker-task cleanup.
    bool try_begin_update_check(bool manual = false);
    void end_update_check();

    // copied status
    bool endpoint_work_claimed() const;
    ExportTaskControlSnapshot control_snapshot() const;
    ExportStatusSnapshot status_snapshot() const;
    ExportSmbStatusSnapshot smb_status() const;
    ExportSleepHqStatusSnapshot sleephq_status() const;
#if AC_STACK_PROFILE_ENABLED
    uint32_t stack_high_water_bytes() const;
#endif

private:
    enum WorkReservation : uint8_t {
        ExportDemand = 1,
        EndpointStep = 2,
        UpdateCheck = 4,
        EndpointActive = 8,
    };

    struct Command {
        ExportRequest request;
        uint32_t sequence = 0;
    };

    struct PublishedInputs {
        ExportEndpointConfig config;
        ActivitySnapshot activity;
        NetworkSnapshot network;
        uint32_t config_generation = 1;
        uint32_t activity_generation = 0;
        uint32_t network_generation = 0;
        uint32_t defer_generation = 0;
        std::array<uint32_t, AC_EXPORT_ENDPOINT_CAPACITY> defer_until_ms = {};
        Command command;
    };

    struct Runtime;

    // task loop
    static void task_entry(void *context);
    void run();
    ExportStep step_endpoint();
    bool try_begin_endpoint_step();

    // published inputs
    bool lock_inputs(uint32_t timeout_ms = 20) const;
    void unlock_inputs() const;
    bool copy_inputs(PublishedInputs &out) const;
    void apply_inputs(const PublishedInputs &inputs, uint32_t now_ms);
    bool apply_command(const Command &command);
    void finish_command(const Command &command, bool failed);
    void wake();

    // endpoint scheduling policy
    void update_network_ready(const NetworkSnapshot &network,
                              uint32_t now_ms);
    bool runtime_blocked(const ActivitySnapshot &activity) const;

    // status publication
    bool publish_work_claim();
    void publish_status();

    Runtime *runtime_ = nullptr;
    TaskHandle_t task_ = nullptr;

    mutable StaticSemaphore_t input_lock_storage_ = {};
    mutable SemaphoreHandle_t input_lock_ = nullptr;

    mutable StaticSemaphore_t status_lock_storage_ = {};
    mutable SemaphoreHandle_t status_lock_ = nullptr;

    uint32_t applied_config_generation_ = 0;
    uint32_t applied_activity_generation_ = 0;
    uint32_t applied_network_generation_ = 0;
    uint32_t applied_defer_generation_ = 0;
    uint32_t network_ready_since_ms_ = 0;
    bool network_ready_ = false;
    bool runtime_blocked_ = true;
    size_t next_idle_endpoint_ = 0;
    uint32_t next_command_sequence_ = 1;
    std::atomic<uint8_t> work_reservation_{0};
    std::atomic<bool> storage_suspend_requested_{false};
    std::atomic<bool> storage_paused_{false};
};

}  // namespace aircannect
