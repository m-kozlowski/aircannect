#pragma once

#include "http_route_module.h"

class AsyncWebServerRequest;

namespace aircannect {

class ResmedFirmwareRepository;

class ResmedFirmwareHttpController final : public HttpRouteModule {
public:
    bool begin(ResmedFirmwareRepository &repository);
    void register_routes(AsyncWebServer &server) override;

private:
    void send_catalog(AsyncWebServerRequest *request) const;
    void request_refresh(AsyncWebServerRequest *request) const;
    void request_remove(AsyncWebServerRequest *request) const;

    ResmedFirmwareRepository *repository_ = nullptr;
};

}  // namespace aircannect
