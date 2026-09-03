#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "http_route_module.h"
#include "large_text_buffer.h"
#include "main_loop_inbox.h"
#include "ota_status.h"
#include "resmed_firmware_image.h"

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

    uint32_t snapshot_revision() const { return snapshot_revision_; }
    bool copy_snapshot(LargeTextBuffer &out, uint32_t &revision) const;

private:
    enum class CommandKind : uint8_t {
        ResmedInit,
        ResmedBlock,
        ResmedInstall,
        ResmedDump,
        ResmedDumpConfirm,
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
        ResmedFirmwareTarget resmed_target =
            AC_RESMED_FIRMWARE_DEFAULT_TARGET;
        ResmedFirmwareInstallTransport resmed_transport =
            AC_RESMED_FIRMWARE_DEFAULT_TRANSPORT;
    };

    static constexpr size_t CommandQueueDepth = 8;
    static constexpr size_t CommandsPerPoll = 4;

    bool enqueue(Command &&command);
    void execute(Command &command);
    void send_queue_result(AsyncWebServerRequest *request, bool queued) const;
    void request_snapshot();
    void publish_snapshot_if_needed(bool force = false);

    static constexpr uint32_t SnapshotActiveIntervalMs = 250;
    static constexpr uint32_t SnapshotIdleIntervalMs = 3000;

    FirmwareInstaller *installer_ = nullptr;
    FirmwareUrlSource *url_source_ = nullptr;
    ArduinoOtaSource *arduino_source_ = nullptr;
    UpdateChecker *update_checker_ = nullptr;
    ResmedFirmwarePreparer *resmed_preparer_ = nullptr;
    ResmedOtaManager *resmed_ota_ = nullptr;

    MainLoopInbox<Command, CommandQueueDepth> commands_;

    LargeTextBuffer snapshot_json_;
    LargeTextBuffer snapshot_build_json_;
    OtaStatusSnapshot published_status_;
    StaticSemaphore_t snapshot_mutex_storage_ = {};
    SemaphoreHandle_t snapshot_mutex_ = nullptr;
    std::atomic<bool> snapshot_requested_{false};
    uint32_t snapshot_revision_ = 0;
    uint32_t next_snapshot_ms_ = 0;
    bool snapshot_initialized_ = false;
};

}  // namespace aircannect
