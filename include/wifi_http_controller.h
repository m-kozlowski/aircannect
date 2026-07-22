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

class WifiManager;

class WifiHttpController final : public HttpRouteModule {
public:
    bool begin(WifiManager &wifi);
    void register_routes(AsyncWebServer &server) override;
    void poll();

private:
    enum class CommandKind : uint8_t {
        Set,
        Add,
        Remove,
        Clear,
        Reconnect,
    };

    struct Command {
        CommandKind kind = CommandKind::Reconnect;
        std::string ssid;
        std::string password;
        size_t index = 0;
    };

    static constexpr size_t CommandQueueDepth = 4;
    static constexpr size_t CommandsPerPoll = 2;

    bool enqueue(Command &&command);
    void execute(Command &command);
    bool publish_snapshot();

    void send_snapshot(AsyncWebServerRequest *request) const;
    void send_update(AsyncWebServerRequest *request);

    WifiManager *wifi_ = nullptr;
    MainLoopInbox<Command, CommandQueueDepth> commands_;

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    LargeTextBuffer snapshot_json_;
    uint32_t last_snapshot_ms_ = 0;
    bool snapshot_dirty_ = true;
};

}  // namespace aircannect
