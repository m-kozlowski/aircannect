#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "http_route_module.h"
#include "main_loop_inbox.h"

class AsyncWebServerRequest;

namespace aircannect {

class ArduinoOtaSource;
class FirmwareInstaller;
class FirmwareUrlSource;
class ResmedFirmwarePreparer;
class ResmedOtaManager;
class UpdateChecker;

// Owns OTA HTTP transport and defers RPC-backed ResMed commands to the main
// loop. Firmware lifecycle policy remains in the two OTA managers.
class OtaHttpController final : public HttpRouteModule {
public:
    bool begin(FirmwareInstaller &installer,
               FirmwareUrlSource &url_source,
               ArduinoOtaSource &arduino_source,
               UpdateChecker &update_checker,
               ResmedFirmwarePreparer &resmed_preparer,
               ResmedOtaManager &resmed_ota);
    void register_routes(AsyncWebServer &server) override;
    void poll();

private:
    enum class CommandKind : uint8_t {
        ResmedInit,
        ResmedBlock,
        ResmedInstall,
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
        std::string path;
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

    FirmwareInstaller *installer_ = nullptr;
    FirmwareUrlSource *url_source_ = nullptr;
    ArduinoOtaSource *arduino_source_ = nullptr;
    UpdateChecker *update_checker_ = nullptr;
    ResmedFirmwarePreparer *resmed_preparer_ = nullptr;
    ResmedOtaManager *resmed_ota_ = nullptr;

    MainLoopInbox<Command, CommandQueueDepth> commands_;
};

}  // namespace aircannect
