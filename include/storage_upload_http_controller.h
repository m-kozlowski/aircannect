#pragma once

#include <atomic>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "http_route_module.h"
#include "runtime_snapshots.h"

class AsyncWebServerRequest;

namespace aircannect {

class StorageStatusPort;
class StorageUploadPort;

// Presents the bounded storage upload port over authenticated HTTP.
class StorageUploadHttpController final : public HttpRouteModule {
public:
    bool begin(StorageUploadPort &upload_port,
               StorageStatusPort &status_port);
    void register_routes(AsyncWebServer &server) override;

    void publish_activity(const ActivitySnapshot &activity);

private:
    // HTTP operations
    void send_start(AsyncWebServerRequest *request);
    void receive_chunk(AsyncWebServerRequest *request,
                       uint8_t *data,
                       size_t length,
                       size_t index,
                       size_t total);
    void send_chunk_result(AsyncWebServerRequest *request);
    void send_status(AsyncWebServerRequest *request);
    void send_cancel(AsyncWebServerRequest *request);

    // Chunk capability
    bool issue_capability(uint32_t id, char token_out[33]);
    bool authorize_chunk(uint32_t id, const char *token);
    void clear_capability(uint32_t id = 0);

    StorageUploadPort *upload_port_ = nullptr;
    StorageStatusPort *status_port_ = nullptr;

    mutable StaticSemaphore_t capability_mutex_storage_ = {};
    mutable SemaphoreHandle_t capability_mutex_ = nullptr;
    char capability_token_[33] = {};
    uint32_t capability_upload_id_ = 0;
    uint32_t capability_last_used_ms_ = 0;
    std::atomic<uint32_t> next_generation_{0};

    std::atomic<bool> therapy_active_{false};
};

}  // namespace aircannect
