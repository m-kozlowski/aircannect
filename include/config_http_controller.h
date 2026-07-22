#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "http_route_module.h"
#include "large_text_buffer.h"
#include "main_loop_inbox.h"

class AsyncWebServerRequest;

namespace aircannect {

class ConfigService;

// Owns config HTTP serialization and submits complete updates to the sole
// ConfigService transaction owner from the main loop.
class ConfigHttpController final : public HttpRouteModule {
public:
    bool begin(ConfigService &config);
    void register_routes(AsyncWebServer &server) override;
    void poll();

private:
    struct Command {
        std::string body;
    };

    static constexpr size_t SectionCount = 9;
    static constexpr size_t CommandQueueDepth = 4;
    static constexpr size_t CommandsPerPoll = 2;

    bool enqueue(Command &&command);
    void execute(Command &command);
    bool publish_snapshots();

    void send_config(AsyncWebServerRequest *request,
                     const char *section) const;
    void send_schema(AsyncWebServerRequest *request) const;
    void send_update(AsyncWebServerRequest *request);

    ConfigService *config_ = nullptr;
    MainLoopInbox<Command, CommandQueueDepth> commands_;

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    LargeTextBuffer all_json_;
    LargeTextBuffer section_json_[SectionCount];
    LargeTextBuffer schema_json_;
    uint32_t published_revision_ = 0;
};

}  // namespace aircannect
