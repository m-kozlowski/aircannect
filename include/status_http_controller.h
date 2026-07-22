#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "http_route_module.h"
#include "large_text_buffer.h"

class AsyncWebServerRequest;

namespace aircannect {

class As11DeviceService;
class ConfigService;
class FirmwareInstaller;
class OximetryHub;
class PlxPeripheral;
class TimeSyncService;
class UdpOximeterSource;
class UpdateChecker;
class WifiManager;

class StatusHttpController final : public HttpRouteModule {
public:
    using PollCheckpoint = void (*)(const char *section);

    bool begin(As11DeviceService &device,
               WifiManager &wifi,
               ConfigService &config,
               TimeSyncService &time_sync,
               FirmwareInstaller &installer,
               UpdateChecker &update_checker,
               OximetryHub &oximetry_hub,
               UdpOximeterSource &oximetry_udp,
               PlxPeripheral &plx_peripheral);
    void register_routes(AsyncWebServer &server) override;
    void poll(PollCheckpoint checkpoint = nullptr);

    uint32_t revision() const { return revision_; }
    bool copy_snapshot(LargeTextBuffer &out, uint32_t &revision) const;

private:
    bool publish_snapshot(PollCheckpoint checkpoint = nullptr);
    void send_snapshot(AsyncWebServerRequest *request) const;

    As11DeviceService *device_ = nullptr;
    WifiManager *wifi_ = nullptr;
    ConfigService *config_ = nullptr;
    TimeSyncService *time_sync_ = nullptr;
    FirmwareInstaller *installer_ = nullptr;
    UpdateChecker *update_checker_ = nullptr;
    OximetryHub *oximetry_hub_ = nullptr;
    UdpOximeterSource *oximetry_udp_ = nullptr;
    PlxPeripheral *plx_peripheral_ = nullptr;

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    LargeTextBuffer snapshot_json_;
    LargeTextBuffer build_json_;

    uint32_t observed_device_revision_ = 0;
    uint32_t observed_config_revision_ = 0;
    uint32_t last_snapshot_ms_ = 0;
    uint32_t revision_ = 0;
    bool snapshot_dirty_ = true;
};

}  // namespace aircannect
