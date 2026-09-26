#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "http_route_module.h"
#include "large_text_buffer.h"

class AsyncWebServerRequest;

namespace aircannect {

class LiveChartService;

struct LiveHttpMemoryStatus {
    size_t live_length = 0;
    size_t live_capacity = 0;
};

class LiveHttpController final : public HttpRouteModule {
public:
    bool begin(LiveChartService &live);
    void stop();
    void register_routes(HttpRouteRegistry &server) override;
    void poll(size_t connected_sse_clients,
              size_t healthy_sse_clients,
              uint32_t now_ms);

    bool live_payload(const char *&data,
                      size_t &length,
                      uint32_t &generation) const;
    LiveHttpMemoryStatus memory_status() const;

private:
    struct LiveViewLease {
        uint32_t client_hash = 0;
        uint32_t expires_ms = 0;
    };

    bool live_view_requested(uint32_t now_ms);
    void publish_live_payload(uint32_t now_ms);

    void send_live_view_state(AsyncWebServerRequest *request);

    LiveChartService *live_ = nullptr;

    StaticSemaphore_t lease_mutex_storage_ = {};
    SemaphoreHandle_t lease_mutex_ = nullptr;

    LiveViewLease leases_[AC_WEB_SSE_CLIENTS_MAX + 1];
    LargeTextBuffer live_json_;

    uint32_t live_generation_ = 0;
    uint32_t last_live_send_ms_ = 0;
    uint32_t live_backpressure_since_ms_ = 0;
    bool live_backpressure_active_ = false;
};

}  // namespace aircannect
