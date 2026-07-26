#pragma once

#include <atomic>
#include <memory>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "http_route_module.h"
#include "runtime_snapshots.h"

class AsyncWebServerRequest;

namespace aircannect {

class StorageArchivePort;
class StorageBrowserPort;
class StorageDeletePort;
class StorageReadPort;
class StorageStatusPort;

// Presents storage-owned browser, archive, delete, and file-log operations.
class StorageHttpController final : public HttpRouteModule {
public:
    StorageHttpController();
    ~StorageHttpController();

    bool begin(StorageReadPort &read_port,
               StorageBrowserPort &browser_port,
               StorageArchivePort &archive_port,
               StorageDeletePort &delete_port,
               StorageStatusPort &status_port);
    void poll();
    void register_routes(AsyncWebServer &server) override;

    void publish_activity(const ActivitySnapshot &activity);

private:
    void poll_file_log_tail();
    void poll_archive_download();

    // storage browser and maintenance
    void send_storage_list(AsyncWebServerRequest *request) const;
    void send_storage_download(AsyncWebServerRequest *request) const;
    void send_file_log_tail(AsyncWebServerRequest *request, size_t lines);
    void send_storage_archive_start(AsyncWebServerRequest *request) const;
    void send_storage_archive_status(AsyncWebServerRequest *request) const;
    void send_storage_archive_download(AsyncWebServerRequest *request);
    void send_storage_delete_start(AsyncWebServerRequest *request) const;
    void send_storage_delete_status(AsyncWebServerRequest *request) const;

    StorageReadPort *storage_read_ = nullptr;
    StorageBrowserPort *storage_browser_ = nullptr;
    StorageArchivePort *storage_archive_ = nullptr;
    StorageDeletePort *storage_delete_ = nullptr;
    StorageStatusPort *storage_status_ = nullptr;

    struct PendingFileLogTail;
    struct PendingArchiveDownload;
    std::unique_ptr<PendingFileLogTail> pending_file_log_tail_;
    std::unique_ptr<PendingArchiveDownload> pending_archive_download_;

    mutable StaticSemaphore_t job_mutex_storage_ = {};
    mutable SemaphoreHandle_t job_mutex_ = nullptr;
    std::atomic<bool> therapy_active_{false};
};

}  // namespace aircannect
