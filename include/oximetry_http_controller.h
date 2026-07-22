#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "http_route_module.h"
#include "large_text_buffer.h"
#include "main_loop_inbox.h"
#include "oximetry_manager.h"

class AsyncWebServerRequest;

namespace aircannect {

class ConfigService;

class OximetryHttpController final : public HttpRouteModule {
public:
    bool begin(OximetryManager &oximetry, ConfigService &config);
    void register_routes(AsyncWebServer &server) override;
    void poll();

private:
    enum class CommandKind : uint8_t {
        Enable,
        Disable,
        PairStart,
        PairStop,
        ForgetBonds,
        AdvertiseStart,
        AdvertiseStop,
        SensorScan,
        SensorDisconnect,
        SensorConnect,
        SensorForget,
        SensorAutoconnect,
    };

    struct Command {
        CommandKind kind = CommandKind::SensorScan;
        std::string target;
        OximetrySensorDevice device;
        bool has_device = false;
        bool enabled = false;
    };

    static constexpr size_t CommandQueueDepth = 4;
    static constexpr size_t CommandsPerPoll = 2;

    bool enqueue(Command &&command);
    void execute(Command &command);
    bool publish_snapshot();

    void send_snapshot(AsyncWebServerRequest *request) const;
    void send_action(AsyncWebServerRequest *request);

    OximetryManager *oximetry_ = nullptr;
    ConfigService *config_ = nullptr;
    MainLoopInbox<Command, CommandQueueDepth> commands_;

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    LargeTextBuffer snapshot_json_;
    uint32_t last_snapshot_ms_ = 0;
    bool snapshot_dirty_ = true;
};

}  // namespace aircannect
