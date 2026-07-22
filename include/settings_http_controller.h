#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "fixed_queue.h"
#include "http_route_module.h"
#include "large_text_buffer.h"

class AsyncWebServerRequest;

namespace aircannect {

class As11DeviceService;
class As11SettingsManager;
class RpcRequestPort;

// Publishes main-loop-owned clinical settings as immutable JSON snapshots.
// HTTP callbacks only select a profile or enqueue typed refresh/write work.
class SettingsHttpController final : public HttpRouteModule {
public:
    bool begin(RpcRequestPort &rpc,
               As11DeviceService &device,
               As11SettingsManager &settings);
    void register_routes(AsyncWebServer &server) override;
    void poll();

private:
    enum class CommandKind : uint8_t {
        Refresh,
        Update,
    };

    struct Command {
        CommandKind kind = CommandKind::Refresh;
        std::string body;
    };

    static constexpr size_t CommandQueueDepth = 8;
    static constexpr size_t CommandsPerPoll = 4;

    bool enqueue(Command &&command);
    void drain_commands();
    void execute(Command &command);

    void publish_snapshot_if_needed();
    void send_catalog(AsyncWebServerRequest *request) const;
    void send_settings(AsyncWebServerRequest *request,
                       int requested_mode,
                       bool refresh_requested);

    RpcRequestPort *rpc_ = nullptr;
    As11DeviceService *device_ = nullptr;
    As11SettingsManager *settings_ = nullptr;

    FixedQueue<Command, CommandQueueDepth> command_queue_;
    StaticSemaphore_t command_mutex_storage_ = {};
    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t command_mutex_ = nullptr;
    SemaphoreHandle_t cache_mutex_ = nullptr;

    LargeTextBuffer catalog_json_;
    LargeTextBuffer settings_json_;
    int requested_mode_ = -1;
    int cached_request_mode_ = -1;
    uint32_t request_generation_ = 1;
    uint32_t observed_settings_revision_ = UINT32_MAX;
    uint32_t observed_device_revision_ = UINT32_MAX;
    bool observed_refresh_pending_ = false;
    bool cached_refresh_pending_ = false;
    bool snapshot_pending_ = true;
};

}  // namespace aircannect
