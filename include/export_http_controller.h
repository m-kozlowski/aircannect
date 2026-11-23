#pragma once

#include "http_route_module.h"

class AsyncWebServerRequest;

namespace aircannect {

class ExportCoordinator;
class ReportTask;
class BleSensorSource;
class LargeTextBuffer;

class ExportHttpController final : public HttpRouteModule {
public:
    bool begin(ExportCoordinator &coordinator, ReportTask &reports, BleSensorSource &sensors);
    void register_routes(HttpRouteRegistry &server) override;

    bool build_status_snapshot(LargeTextBuffer &json) const;

private:
    void send_smb_sync_start(AsyncWebServerRequest *request) const;
    void send_smb_sync_verify(AsyncWebServerRequest *request) const;
    void send_smb_sync_status(AsyncWebServerRequest *request) const;

    void send_sleephq_sync_start(AsyncWebServerRequest *request) const;
    void send_sleephq_sync_check(AsyncWebServerRequest *request) const;
    void send_sleephq_sync_status(AsyncWebServerRequest *request) const;

    ExportCoordinator *coordinator_ = nullptr;
    ReportTask *reports_ = nullptr;
    BleSensorSource *sensors_ = nullptr;
};

}  // namespace aircannect
