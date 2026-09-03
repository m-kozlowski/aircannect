#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include <string>

#include "app_config_registry.h"
#include "http_route_module.h"
#include "large_text_buffer.h"
#include "main_loop_inbox.h"
#include "published_json_snapshot.h"

class AsyncWebServerRequest;

namespace aircannect {

class ConfigService;

// Owns config HTTP serialization and submits complete updates to the sole
// ConfigService transaction owner from the main loop.
class ConfigHttpController final : public HttpRouteModule {
public:
    bool begin(ConfigService &config);
    void register_routes(AsyncWebServer &server) override;
    void poll();

    uint32_t update_revision() const {
        return update_snapshot_.revision();
    }
    bool copy_update_snapshot(LargeTextBuffer &out,
                              uint32_t &revision) const;

private:
    enum class CommandKind : uint8_t {
        Update,
        CompleteOnboarding,
    };

    struct Command {
        CommandKind kind = CommandKind::Update;
        uint32_t update_revision = 0;
        std::string body;
        std::string onboarding_user;
        std::string onboarding_password;
        bool onboarding_user_set = false;
        bool onboarding_password_set = false;
    };

    static constexpr size_t SectionCount = AC_CONFIG_GROUP_COUNT;
    static constexpr size_t CommandQueueDepth = 4;
    static constexpr size_t CommandsPerPoll = 1;

    bool enqueue(Command &&command);
    void execute(Command &command);
    uint32_t next_update_revision();
    void publish_update_result(uint32_t update_revision,
                               bool persisted,
                               uint32_t config_revision,
                               const char *error = nullptr);
    bool publish_snapshots();

    void send_config(AsyncWebServerRequest *request,
                     const char *section) const;
    void send_snapshot(AsyncWebServerRequest *request,
                       const LargeTextBuffer &json) const;
    void send_schema(AsyncWebServerRequest *request) const;
    void send_update(AsyncWebServerRequest *request);
    void send_onboarding_complete(AsyncWebServerRequest *request);

    ConfigService *config_ = nullptr;
    MainLoopInbox<Command, CommandQueueDepth> commands_;

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    LargeTextBuffer all_json_;
    LargeTextBuffer section_json_[SectionCount];
    LargeTextBuffer schema_json_;
    PublishedJsonSnapshot update_snapshot_;
    uint32_t published_revision_ = 0;
    std::atomic<uint32_t> next_update_revision_{1};
};

}  // namespace aircannect
