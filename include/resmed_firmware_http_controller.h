#pragma once

#include <stdint.h>

#include "http_route_module.h"
#include "large_text_buffer.h"
#include "published_json_snapshot.h"

class AsyncWebServerRequest;

namespace aircannect {

class ResmedFirmwareRepository;

class ResmedFirmwareHttpController final : public HttpRouteModule {
public:
    bool begin(ResmedFirmwareRepository &repository);
    void register_routes(AsyncWebServer &server) override;
    void poll();

    uint32_t status_snapshot_revision() const {
        return status_snapshot_.revision();
    }
    bool copy_status_snapshot(LargeTextBuffer &out,
                              uint32_t &revision) const;

private:
    bool publish_status_snapshot(bool force = false);
    void send_catalog(AsyncWebServerRequest *request) const;
    void request_refresh(AsyncWebServerRequest *request) const;
    void request_remove(AsyncWebServerRequest *request) const;

    ResmedFirmwareRepository *repository_ = nullptr;

    PublishedJsonSnapshot status_snapshot_;
    LargeTextBuffer status_build_json_;
    uint32_t observed_repository_generation_ = 0;
};

}  // namespace aircannect
