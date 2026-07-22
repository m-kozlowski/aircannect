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

class SinkManager;
class StreamBroker;

struct LiveHttpMemoryStatus {
    size_t stream_length = 0;
    size_t stream_capacity = 0;
    size_t live_length = 0;
    size_t live_capacity = 0;
};

class LiveHttpController final : public HttpRouteModule {
public:
    bool begin(StreamBroker &stream, SinkManager &sink);
    void stop();
    void register_routes(AsyncWebServer &server) override;
    void poll(size_t healthy_sse_clients, uint32_t now_ms);

    bool stream_payload(const char *&data, size_t &length) const;
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
    bool publish_stream_snapshot();
    void publish_live_payload(uint32_t now_ms);

    void send_stream_snapshot(AsyncWebServerRequest *request) const;
    void send_live_view_state(AsyncWebServerRequest *request);

    StreamBroker *stream_ = nullptr;
    SinkManager *sink_ = nullptr;

    StaticSemaphore_t cache_mutex_storage_ = {};
    StaticSemaphore_t lease_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    SemaphoreHandle_t lease_mutex_ = nullptr;

    LiveViewLease leases_[AC_WEB_SSE_CLIENTS_MAX + 1];
    LargeTextBuffer stream_json_;
    LargeTextBuffer stream_build_json_;
    LargeTextBuffer live_json_;

    uint32_t live_generation_ = 0;
    uint32_t last_stream_snapshot_ms_ = 0;
    uint32_t last_live_send_ms_ = 0;
};

}  // namespace aircannect
