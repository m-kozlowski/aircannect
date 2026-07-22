#pragma once

#include "http_route_module.h"

class AsyncWebServerRequest;

namespace aircannect {

class ExportCoordinator;

class ExportHttpController final : public HttpRouteModule {
public:
    bool begin(ExportCoordinator &coordinator);
    void register_routes(AsyncWebServer &server) override;

private:
    void send_smb_sync_start(AsyncWebServerRequest *request) const;
    void send_smb_sync_verify(AsyncWebServerRequest *request) const;
    void send_smb_sync_status(AsyncWebServerRequest *request) const;

    void send_sleephq_sync_start(AsyncWebServerRequest *request) const;
    void send_sleephq_sync_check(AsyncWebServerRequest *request) const;
    void send_sleephq_sync_status(AsyncWebServerRequest *request) const;

    ExportCoordinator *coordinator_ = nullptr;
};

}  // namespace aircannect
