#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "http_route_module.h"
#include "large_text_buffer.h"

class AsyncWebServerRequest;

namespace aircannect {

struct SystemStatusSnapshot;

class StatusHttpController final : public HttpRouteModule {
public:
    bool begin();
    void register_routes(AsyncWebServer &server) override;

    bool refresh_due(uint32_t device_revision,
                     uint32_t config_revision,
                     uint32_t now_ms) const;
    bool publish_snapshot(const SystemStatusSnapshot &snapshot,
                          const char *hostname,
                          uint32_t device_revision,
                          uint32_t config_revision);

    uint32_t revision() const { return revision_; }
    bool copy_snapshot(LargeTextBuffer &out, uint32_t &revision) const;

private:
    void send_snapshot(AsyncWebServerRequest *request) const;

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    LargeTextBuffer snapshot_json_;
    LargeTextBuffer build_json_;

    uint32_t observed_device_revision_ = 0;
    uint32_t observed_config_revision_ = 0;
    uint32_t last_snapshot_ms_ = 0;
    uint32_t revision_ = 0;
    bool snapshot_ready_ = false;
};

}  // namespace aircannect
