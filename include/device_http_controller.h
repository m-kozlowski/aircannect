#pragma once

#include <atomic>
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

class As11DeviceService;
class As11BleRpcLink;
class RpcRequestPort;
class TimeSyncService;

class DeviceHttpController final : public HttpRouteModule {
public:
    bool begin(RpcRequestPort &rpc,
               As11DeviceService &device,
               TimeSyncService &time_sync,
               As11BleRpcLink &ble_link);
    void register_routes(AsyncWebServer &server) override;
    void poll();

    uint32_t ble_pairing_revision() const {
        return published_ble_pairing_revision_;
    }
    bool copy_ble_pairing_snapshot(LargeTextBuffer &out,
                                   uint32_t &revision) const;

private:
    enum class CommandKind : uint8_t {
        TimeNtp,
        TimePush,
        TimePull,
        TimeReset,
        TherapyStart,
        TherapyStop,
        BlePairScan,
        BlePairSelect,
        BlePairPasskey,
        BlePairCancel,
        BlePairForget,
    };

    struct Command {
        CommandKind kind = CommandKind::TimeNtp;
        std::string value;
    };

    static constexpr size_t CommandQueueDepth = 4;
    static constexpr size_t CommandsPerPoll = 2;

    bool enqueue(Command command);
    void execute(Command command);
    bool publish_ble_pairing_snapshot();

    void send_time_action(AsyncWebServerRequest *request);
    void send_therapy_action(AsyncWebServerRequest *request);
    void send_ble_status(AsyncWebServerRequest *request) const;
    void send_ble_action(AsyncWebServerRequest *request);

    RpcRequestPort *rpc_ = nullptr;
    As11DeviceService *device_ = nullptr;
    TimeSyncService *time_sync_ = nullptr;
    As11BleRpcLink *ble_link_ = nullptr;
    MainLoopInbox<Command, CommandQueueDepth> commands_;
    std::atomic<bool> as11_unavailable_{false};

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    LargeTextBuffer ble_pairing_json_;
    LargeTextBuffer ble_pairing_build_json_;
    uint32_t published_ble_pairing_revision_ = 0;
};

}  // namespace aircannect
