#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "as11_device_state.h"
#include "http_route_module.h"
#include "large_text_buffer.h"
#include "main_loop_inbox.h"
#include "published_json_snapshot.h"

class AsyncWebServerRequest;

namespace aircannect {

class As11DeviceService;
class As11SettingsManager;
class RpcRequestPort;

// Publishes main-loop-owned clinical settings as immutable JSON snapshots.
// HTTP callbacks only select a profile or enqueue typed refresh/write work.
class SettingsHttpController final : public HttpRouteModule {
public:
    bool begin(RpcRequestPort &rpc,
               As11DeviceService &device,
               As11SettingsManager &settings);
    void register_routes(HttpRouteRegistry &server) override;
    void poll();

    const PublishedJsonSnapshot &snapshot() const {
        return settings_snapshot_;
    }

private:
    enum class CommandKind : uint8_t {
        Refresh,
        Update,
    };

    struct Command {
        CommandKind kind = CommandKind::Refresh;
        std::string body;
    };

    static constexpr size_t CommandQueueDepth = 8;
    static constexpr size_t CommandsPerPoll = 4;

    // Snapshot assembly stays on the main loop, between CAN drain points.
    enum class BuildPhase : uint8_t { Idle, Settings, Catalog, Composites, Ready };
    static constexpr size_t SnapshotItemsPerPoll = 4;
    static constexpr uint32_t SnapshotBudgetUs = 1000;

    struct SnapshotBuild {
        BuildPhase phase = BuildPhase::Idle;
        size_t index = 0;
        size_t emitted = 0;
        uint32_t settings_revision = 0;
        uint32_t catalog_revision = 0;
        uint32_t device_revision = 0;
        uint32_t request_generation = 0;
        uint32_t now_ms = 0;
        int active_mode = -1;
        int profile_mode = -1;
        As11Availability availability = As11Availability::Unknown;
        bool refresh_pending = false;
        bool advance_revision = false;
    };

    // Commands
    bool enqueue(Command &&command);
    void drain_commands();
    void execute(Command &command);

    // Snapshot assembly and delivery
    void publish_snapshot_if_needed();
    void advance_snapshot_build();
    void send_catalog(AsyncWebServerRequest *request);
    void send_settings(AsyncWebServerRequest *request,
                       int requested_mode,
                       bool refresh_requested);

    RpcRequestPort *rpc_ = nullptr;
    As11DeviceService *device_ = nullptr;
    As11SettingsManager *settings_ = nullptr;

    StaticSemaphore_t cache_mutex_storage_ = {};
    SemaphoreHandle_t cache_mutex_ = nullptr;
    MainLoopInbox<Command, CommandQueueDepth, InboxStorage::Psram> commands_;

    LargeTextBuffer catalog_json_;
    PublishedJsonSnapshot settings_snapshot_;
    LargeTextBuffer settings_build_json_;
    LargeTextBuffer catalog_build_json_;
    SnapshotBuild build_;
    int requested_mode_ = -1;
    uint32_t cached_catalog_revision_ = 0;
    uint32_t request_generation_ = 1;
    int published_active_mode_ = -1;
    As11Availability published_availability_ = As11Availability::Unknown;
    uint32_t observed_settings_revision_ = UINT32_MAX;
    uint32_t observed_device_revision_ = UINT32_MAX;
    As11Availability cached_device_availability_ = As11Availability::Unknown;
    bool observed_refresh_pending_ = false;
    bool cached_refresh_pending_ = false;
    bool snapshot_pending_ = true;
};

}  // namespace aircannect
