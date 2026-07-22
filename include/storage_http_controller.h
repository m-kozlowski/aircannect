#pragma once

#include <atomic>
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
    bool begin(StorageReadPort &read_port,
               StorageBrowserPort &browser_port,
               StorageArchivePort &archive_port,
               StorageDeletePort &delete_port,
               StorageStatusPort &status_port);
    void register_routes(AsyncWebServer &server) override;

    void publish_activity(const ActivitySnapshot &activity);

private:
    // storage browser and maintenance
    void send_storage_list(AsyncWebServerRequest *request) const;
    void send_storage_download(AsyncWebServerRequest *request) const;
    void send_file_log_tail(AsyncWebServerRequest *request,
                            size_t lines) const;
    void send_storage_archive_start(AsyncWebServerRequest *request) const;
    void send_storage_archive_status(AsyncWebServerRequest *request) const;
    void send_storage_archive_download(AsyncWebServerRequest *request) const;
    void send_storage_delete_start(AsyncWebServerRequest *request) const;
    void send_storage_delete_status(AsyncWebServerRequest *request) const;

    StorageReadPort *storage_read_ = nullptr;
    StorageBrowserPort *storage_browser_ = nullptr;
    StorageArchivePort *storage_archive_ = nullptr;
    StorageDeletePort *storage_delete_ = nullptr;
    StorageStatusPort *storage_status_ = nullptr;

    mutable StaticSemaphore_t job_mutex_storage_ = {};
    mutable SemaphoreHandle_t job_mutex_ = nullptr;
    std::atomic<bool> therapy_active_{false};
};

}  // namespace aircannect
