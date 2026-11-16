#pragma once

#include "async_deferred_response.h"
#include "http_route_module.h"
#include "main_loop_inbox.h"
#include "settings_backup_service.h"

namespace aircannect {

// Carries explicit commands/results only. File listing and transfer stay on
// the existing storage endpoints, and settings operations stay in the owner.
class BackupHttpController final : public HttpRouteModule {
public:
    bool begin(SettingsBackupService &service);
    void register_routes(HttpRouteRegistry &server) override;
    void poll();

private:
    struct Command {
        SettingsBackupCommand operation;
        std::shared_ptr<AsyncDeferredResponse::State> response;
    };

    void submit(AsyncWebServerRequest *request);
    void publish_result();

    SettingsBackupService *service_ = nullptr;
    MainLoopInbox<Command, 2, InboxStorage::Psram> commands_;
    std::shared_ptr<AsyncDeferredResponse::State> response_;
};

}  // namespace aircannect
