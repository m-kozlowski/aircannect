#pragma once

#include <atomic>
#include <memory>
#include <stdint.h>

#include "http_route_module.h"
#include "large_text_buffer.h"
#include "main_loop_inbox.h"
#include "operation_outcome.h"
#include "published_json_snapshot.h"

class AsyncWebServer;
class AsyncWebServerRequest;

namespace aircannect {

class ReportTask;
class ReportPreferencesService;
class StorageStreamPort;

// Presents immutable v9 report metadata and bounded signal-file ranges.
// ReportTask owns discovery and materialization; this class only validates
// HTTP requests and transports already published files.
class ReportHttpController final : public HttpRouteModule {
public:
    ReportHttpController();
    ~ReportHttpController();

    void begin(ReportTask &report_task,
               ReportPreferencesService &preferences,
               StorageStreamPort &stream_port);
    void poll();
    void register_routes(HttpRouteRegistry &server) override;

    const PublishedJsonSnapshot &completion_snapshot() const {
        return completion_snapshot_;
    }
    const PublishedJsonSnapshot &preferences_snapshot() const;

    void send_summary(AsyncWebServerRequest *request) const;
    void send_preferences(AsyncWebServerRequest *request) const;
    void send_preferences_update(AsyncWebServerRequest *request);
    void send_result(AsyncWebServerRequest *request);
    void send_plot(AsyncWebServerRequest *request);

private:
    void publish_completion();
    uint32_t next_generation() const;

    struct PendingResponses;
    struct PreferenceCommand {
        uint32_t request_id = 0;
        std::string body;
    };

    void poll_preference_commands();
    uint32_t next_preference_request_id();

    ReportTask *report_task_ = nullptr;
    ReportPreferencesService *preferences_ = nullptr;
    StorageStreamPort *stream_port_ = nullptr;
    std::unique_ptr<PendingResponses> pending_;
    PublishedJsonSnapshot completion_snapshot_;
    LargeTextBuffer completion_json_;
    OperationTicket observed_completion_;
    uint32_t completion_serial_ = 0;
    mutable std::atomic<uint32_t> next_generation_{1};
    mutable std::atomic<uint32_t> next_preference_request_{1};
    MainLoopInbox<PreferenceCommand, 4, InboxStorage::Psram>
        preference_commands_;
    PreferenceCommand pending_preference_command_;
};

}  // namespace aircannect
