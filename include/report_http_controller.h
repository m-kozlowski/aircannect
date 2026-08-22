#pragma once

#include <atomic>
#include <memory>
#include <stdint.h>

#include "http_route_module.h"

class AsyncWebServer;
class AsyncWebServerRequest;

namespace aircannect {

class ReportTask;
enum class ReportArtifactKind : uint8_t;
struct ReportArtifactDescriptor;
class SleepDayId;

// Presents immutable report snapshots and artifacts over HTTP. Report policy
// stays in ReportTask; this class validates requests and serves ready bytes.
class ReportHttpController final : public HttpRouteModule {
public:
    ReportHttpController();
    ~ReportHttpController();

    void begin(ReportTask &report_task);
    void poll();
    void register_routes(AsyncWebServer &server) override;

    void send_summary(AsyncWebServerRequest *request) const;
    void send_result(AsyncWebServerRequest *request);
    void send_plot(AsyncWebServerRequest *request);

private:
    void send_artifact(AsyncWebServerRequest *request,
                       SleepDayId sleep_day,
                       ReportArtifactKind kind,
                       int64_t range_start_ms = 0,
                       int64_t range_end_ms = 0);
    void queue_artifact_response(
        AsyncWebServerRequest *request,
        const ReportArtifactDescriptor &artifact,
        bool prefer_deflate);
    uint32_t next_generation() const;

    struct PendingResponses;

    ReportTask *report_task_ = nullptr;
    std::unique_ptr<PendingResponses> pending_;
    mutable std::atomic<uint32_t> next_generation_{1};
};

}  // namespace aircannect
