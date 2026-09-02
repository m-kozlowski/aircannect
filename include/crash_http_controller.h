#pragma once

#include "http_route_module.h"

class AsyncWebServerRequest;

namespace aircannect {

class CrashDiagnostics;

class CrashHttpController final : public HttpRouteModule {
public:
    explicit CrashHttpController(CrashDiagnostics &diagnostics)
        : diagnostics_(diagnostics) {}

    void register_routes(AsyncWebServer &server) override;

private:
    void send_status(AsyncWebServerRequest *request) const;
    void send_dump(AsyncWebServerRequest *request) const;

    CrashDiagnostics &diagnostics_;
};

}  // namespace aircannect
