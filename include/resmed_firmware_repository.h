#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <memory>
#include <stdint.h>

#include "resmed_firmware_catalog.h"
#include "runtime_snapshots.h"
#include "storage_path_port.h"
#include "storage_scan_port.h"

namespace aircannect {

enum class ResmedFirmwareRepositoryState : uint8_t {
    Idle,
    EnsuringDirectory,
    Scanning,
    Removing,
    Ready,
    Error,
};

struct ResmedFirmwareRepositoryStatus {
    ResmedFirmwareRepositoryState state = ResmedFirmwareRepositoryState::Idle;
    uint32_t revision = 0;
    size_t entries = 0;
    bool truncated = false;
    bool refresh_pending = true;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

const char *resmed_firmware_repository_state_name(
    ResmedFirmwareRepositoryState state);

class ResmedFirmwareRepository {
public:
    ~ResmedFirmwareRepository();

    bool begin(StorageScanPort &scan_port, StoragePathPort &path_port);
    void poll();

    bool request_refresh(bool foreground = true);
    bool request_remove(const char *path);
    void publish_activity(const ActivitySnapshot &activity);

    ResmedFirmwareRepositoryStatus status() const;
    std::shared_ptr<const ResmedFirmwareCatalogSnapshot> snapshot() const;

private:
    static constexpr uint32_t RetryDelayMs = 30000;

    enum class Action : uint8_t {
        None,
        EnsureDirectory,
        Scan,
        Remove,
    };

    bool lock(uint32_t timeout_ms = 20) const;
    void unlock() const;
    uint32_t next_generation();

    void poll_completion();
    void start_pending_operation();
    void publish_status(ResmedFirmwareRepositoryState state,
                        const char *error = nullptr);
    void fail_action(const char *error);
    bool direct_repository_file(const char *path) const;

    StorageScanPort *scan_port_ = nullptr;
    StoragePathPort *path_port_ = nullptr;

    mutable StaticSemaphore_t mutex_storage_ = {};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    ResmedFirmwareRepositoryStatus status_;
    std::shared_ptr<const ResmedFirmwareCatalogSnapshot> snapshot_;
    ActivitySnapshot activity_;
    bool refresh_requested_ = true;
    bool foreground_refresh_ = false;
    bool remove_requested_ = false;
    char remove_path_[AC_STORAGE_PATH_MAX] = {};

    Action action_ = Action::None;
    OperationTicket ticket_;
    bool directory_ready_ = false;
    uint32_t generation_ = 0;
    uint32_t retry_at_ms_ = 0;
};

}  // namespace aircannect
