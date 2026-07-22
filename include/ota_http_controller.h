#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "http_route_module.h"
#include "main_loop_inbox.h"

class AsyncWebServerRequest;

namespace aircannect {

class OtaManager;
class ResmedOtaManager;

// Owns OTA HTTP transport and defers RPC-backed ResMed commands to the main
// loop. Firmware lifecycle policy remains in the two OTA managers.
class OtaHttpController final : public HttpRouteModule {
public:
    bool begin(OtaManager &esp_ota, ResmedOtaManager &resmed_ota);
    void register_routes(AsyncWebServer &server) override;
    void poll();

private:
    enum class CommandKind : uint8_t {
        ResmedInit,
        ResmedBlock,
        ResmedCheck,
        ResmedApplyPlain,
        ResmedApplyAuthenticated,
        ResmedAbort,
    };

    struct Command {
        CommandKind kind = CommandKind::ResmedCheck;
        size_t number = 0;
        bool flag = false;
        std::string data;
        std::string filename;
        std::string sha256;
        std::string authentication;
        std::string confirmation;
    };

    static constexpr size_t CommandQueueDepth = 8;
    static constexpr size_t CommandsPerPoll = 4;

    bool enqueue(Command &&command);
    void execute(Command &command);
    void send_queue_result(AsyncWebServerRequest *request, bool queued) const;

    OtaManager *esp_ota_ = nullptr;
    ResmedOtaManager *resmed_ota_ = nullptr;

    MainLoopInbox<Command, CommandQueueDepth> commands_;
};

}  // namespace aircannect
