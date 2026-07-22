#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
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

struct ExportTaskStatus {
    bool initialized = false;
    bool task_started = false;
    bool network_ready = false;
    bool runtime_blocked = true;
    bool command_pending = false;
    bool active = false;
    bool busy = false;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;
    StorageSyncStatus smb;
    StorageSyncRuntimeStatus smb_runtime;
    SleepHqSyncStatus sleephq;
    SleepHqSyncRuntimeStatus sleephq_runtime;
#if AC_STACK_PROFILE_ENABLED
    uint32_t stack_high_water_words = 0;
#endif
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
    void publish_config(const ExportEndpointConfig &config);
    void publish_activity(const ActivitySnapshot &activity);
    void publish_network(const NetworkSnapshot &network);
    void defer_smb_until(uint32_t until_ms);

    // endpoint commands
    bool request_smb_sync();
    bool request_smb_verify();
    bool request_smb_post_therapy();
    bool request_sleephq_check();
    bool request_sleephq_startup_check();
    bool request_sleephq_sync();
    bool request_sleephq_idle_backfill();
    bool request_sleephq_sync_day(const char *day);
    bool request_sleephq_post_therapy();

    // copied status
    ExportTaskStatus status() const;
#if AC_STACK_PROFILE_ENABLED
    uint32_t stack_high_water_bytes() const;
#endif

private:
    enum class CommandKind : uint8_t {
        None,
        SmbSync,
        SmbVerify,
        SmbPostTherapy,
        SleepHqCheck,
        SleepHqStartupCheck,
        SleepHqSync,
        SleepHqIdleBackfill,
        SleepHqSyncDay,
        SleepHqPostTherapy,
    };

    struct Command {
        CommandKind kind = CommandKind::None;
        uint32_t sequence = 0;
        char day[9] = {};
    };

    struct PublishedInputs {
        ExportEndpointConfig config;
        ActivitySnapshot activity;
        NetworkSnapshot network;
        uint32_t config_generation = 1;
        uint32_t activity_generation = 0;
        uint32_t network_generation = 0;
        uint32_t smb_defer_generation = 0;
        uint32_t smb_defer_until_ms = 0;
        Command command;
    };

    struct Runtime;

    // task loop
    static void task_entry(void *context);
    void run();
    ExportStep step_endpoint();

    // published inputs
    bool lock_inputs(uint32_t timeout_ms = 20) const;
    void unlock_inputs() const;
    bool copy_inputs(PublishedInputs &out) const;
    void apply_inputs(const PublishedInputs &inputs, uint32_t now_ms);
    bool queue_command(CommandKind kind, const char *day = nullptr);
    bool apply_command(const Command &command);
    void finish_command(const Command &command, bool failed);
    void wake();

    // endpoint scheduling policy
    void update_network_ready(const NetworkSnapshot &network,
                              uint32_t now_ms);
    bool runtime_blocked(const ActivitySnapshot &activity) const;

    // status publication
    void publish_status();

    Runtime *runtime_ = nullptr;
    TaskHandle_t task_ = nullptr;

    mutable StaticSemaphore_t input_lock_storage_ = {};
    mutable SemaphoreHandle_t input_lock_ = nullptr;
    PublishedInputs inputs_;

    mutable StaticSemaphore_t status_lock_storage_ = {};
    mutable SemaphoreHandle_t status_lock_ = nullptr;
    ExportTaskStatus status_;

    uint32_t applied_config_generation_ = 0;
    uint32_t applied_activity_generation_ = 0;
    uint32_t applied_network_generation_ = 0;
    uint32_t applied_smb_defer_generation_ = 0;
    uint32_t network_ready_since_ms_ = 0;
    bool network_ready_ = false;
    bool runtime_blocked_ = true;
    bool next_idle_endpoint_smb_ = true;
    uint32_t next_command_sequence_ = 1;
};

}  // namespace aircannect
