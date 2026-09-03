#pragma once

#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "http_route_module.h"
#include "large_text_buffer.h"
#include "published_json_snapshot.h"
#include "runtime_snapshots.h"

class AsyncWebServerRequest;

namespace aircannect {

class StorageArchivePort;
class StorageBrowserPort;
class StorageDeletePort;
class StoragePathPort;
class StorageReadPort;
class StorageStatusPort;

// Presents storage-owned browser, archive, delete, and file-log operations.
class StorageHttpController final : public HttpRouteModule {
public:
    StorageHttpController();
    ~StorageHttpController();

    bool begin(StorageReadPort &read_port,
               StorageBrowserPort &browser_port,
               StoragePathPort &path_port,
               StorageArchivePort &archive_port,
               StorageDeletePort &delete_port,
               StorageStatusPort &status_port);
    void poll();
    void register_routes(AsyncWebServer &server) override;

    void publish_activity(const ActivitySnapshot &activity);

    const PublishedJsonSnapshot &operation_snapshot() const {
        return operation_snapshot_;
    }

private:
    void poll_file_log_tail();
    void poll_archive_download();
    void poll_storage_rename();
    void request_operation_snapshot();
    bool publish_operation_snapshot_if_needed(bool force = false);

    // storage browser and maintenance
    void send_storage_list(AsyncWebServerRequest *request) const;
    void send_storage_download(AsyncWebServerRequest *request) const;
    void send_storage_rename(AsyncWebServerRequest *request);
    void send_file_log_tail(AsyncWebServerRequest *request, size_t lines);
    void send_storage_archive_start(AsyncWebServerRequest *request);
    void send_storage_archive_status(AsyncWebServerRequest *request) const;
    void send_storage_archive_download(AsyncWebServerRequest *request);
    void send_storage_delete_start(AsyncWebServerRequest *request);
    void send_storage_delete_status(AsyncWebServerRequest *request) const;

    StorageReadPort *storage_read_ = nullptr;
    StorageBrowserPort *storage_browser_ = nullptr;
    StoragePathPort *storage_path_ = nullptr;
    StorageArchivePort *storage_archive_ = nullptr;
    StorageDeletePort *storage_delete_ = nullptr;
    StorageStatusPort *storage_status_ = nullptr;

    struct PendingFileLogTail;
    struct PendingArchiveDownload;
    struct PendingStorageRename;
    std::unique_ptr<PendingFileLogTail> pending_file_log_tail_;
    std::unique_ptr<PendingArchiveDownload> pending_archive_download_;
    std::unique_ptr<PendingStorageRename> pending_storage_rename_;
    uint32_t storage_rename_generation_ = 0;

    // Archive and delete status publication
    static constexpr uint32_t OperationSnapshotActiveIntervalMs = 500;
    static constexpr uint32_t OperationSnapshotIdleIntervalMs = 3000;
    PublishedJsonSnapshot operation_snapshot_;
    LargeTextBuffer operation_snapshot_build_json_;
    std::atomic<bool> operation_snapshot_requested_{false};
    uint32_t next_operation_snapshot_ms_ = 0;
    bool operation_snapshot_active_ = false;
    bool operation_snapshot_initialized_ = false;

    // Request admission
    mutable StaticSemaphore_t job_mutex_storage_ = {};
    mutable SemaphoreHandle_t job_mutex_ = nullptr;
    std::atomic<bool> therapy_active_{false};
};

}  // namespace aircannect
