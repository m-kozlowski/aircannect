#pragma once

#include <Arduino.h>
#include <atomic>
#include <stdint.h>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "app_config.h"
#include "fixed_queue.h"
#include "large_text_buffer.h"
#include "management_console.h"

class AsyncWebServerRequest;
class AsyncEventSourceClient;
class AsyncEventSource;
class AsyncWebServer;

namespace aircannect {

class ConfigHttpController;
class DeviceHttpController;
class HttpRouteModule;
class ExportHttpController;
class LiveHttpController;
class OximetryHttpController;
class OtaHttpController;
class PublishedJsonSnapshot;
class ReportHttpController;
class ResmedFirmwareHttpController;
class SettingsHttpController;
class StatusHttpController;
class StorageHttpController;

enum WebCommandKind : uint8_t {
    WebCommandConsoleLine,
    WebCommandConsoleClear,
};

struct WebCommand {
    uint8_t kind = WebCommandConsoleLine;
    std::string text;
};

struct WebUiBufferMemoryStatus {
    size_t length = 0;
    size_t capacity = 0;
};

struct WebUiMemoryStatus {
    bool started = false;
    WebUiBufferMemoryStatus status;
    WebUiBufferMemoryStatus exports;
    WebUiBufferMemoryStatus config;
    WebUiBufferMemoryStatus as11_ble;
    WebUiBufferMemoryStatus oximetry;
    WebUiBufferMemoryStatus settings;
    WebUiBufferMemoryStatus ota;
    WebUiBufferMemoryStatus resmed_ota;
    WebUiBufferMemoryStatus resmed_repository;
    WebUiBufferMemoryStatus storage_operation;
    WebUiBufferMemoryStatus report;
    WebUiBufferMemoryStatus stream;
    WebUiBufferMemoryStatus console;
    WebUiBufferMemoryStatus live;
    size_t console_log_length = 0;
    size_t sse_clients = 0;
    size_t sse_pending_total = 0;
    size_t sse_pending_worst = 0;
};

class WebUI {
public:
    using PollCheckpoint = void (*)(const char *section);

    // lifecycle
    bool begin(StatusHttpController &status,
               ExportHttpController &exports,
               ConfigHttpController &config_http,
               DeviceHttpController &device_http,
               OximetryHttpController &oximetry_http,
               SettingsHttpController &settings_http,
               OtaHttpController &ota_http,
               ResmedFirmwareHttpController &resmed_firmware_http,
               StorageHttpController &storage_http,
               ReportHttpController &report_http,
               LiveHttpController &live,
               ConsoleCommandRouter &console_router,
               const AppConfigData &config,
               HttpRouteModule *const *route_modules,
               size_t route_module_count,
               uint16_t port = 80);
    void stop();
    void poll(PollCheckpoint checkpoint = nullptr);
    void apply_auth_config(const AppConfigData &config);

    // inbound device events
    void handle_event(const RpcEvent &event);

    // status
    bool started() const { return started_; }
    WebUiMemoryStatus memory_status();

private:
    // Server setup and cached snapshots
    void register_routes(HttpRouteModule *const *route_modules,
                         size_t route_module_count);
    void bind_snapshot_channels(
        ConfigHttpController &config_http,
        DeviceHttpController &device_http,
        OximetryHttpController &oximetry_http,
        SettingsHttpController &settings_http,
        OtaHttpController &ota_http,
        ResmedFirmwareHttpController &resmed_firmware_http,
        StorageHttpController &storage_http,
        ReportHttpController &report_http);
    void reserve_cached_json();
    void reserve_console_log();
    void append_console_log(const String &text);
    void clear_console_log();
    uint64_t console_log_begin_pos() const;
    void append_console_log_json_range(LargeTextBuffer &json,
                                       uint64_t from,
                                       uint64_t to) const;
    void build_console_json(LargeTextBuffer &json) const;
    void build_console_sse_json(LargeTextBuffer &json) const;
    void note_console_sse_sent();
    void send_console_snapshot(AsyncWebServerRequest *request) const;
    void mark_snapshots_dirty(uint16_t mask);
    void request_sse_push();
    void publish_snapshots(bool force,
                           PollCheckpoint checkpoint = nullptr);

    // Deferred command queue
    bool enqueue_command(WebCommand &&command);
    bool enqueue_simple_command(uint8_t kind);
    void drain_commands();
    void execute_command(WebCommand &command);
    void execute_console_line(const std::string &line);

    // SSE client tracking
    void handle_sse_connect(AsyncEventSourceClient *client);
    void handle_sse_disconnect(AsyncEventSourceClient *client);
    void enforce_sse_limits();
    void count_sse_clients(size_t &connected, size_t &healthy);
    enum class SseSendResult : uint8_t { Skipped, Sent, Failed };
    SseSendResult send_sse_to_clients(const char *payload, const char *event,
                                      uint32_t id, bool status_heartbeat);
    SseSendResult send_snapshot_to_clients(size_t channel_index,
                                           uint32_t id);

    // Dashboard live stream transport
    void poll_live_transport(size_t connected_clients,
                             size_t healthy_clients);

    // Response helpers
    String queued_json(const char *result = "queued") const;
    void send_queue_result(AsyncWebServerRequest *request,
                           bool queued,
                           const char *result = "queued") const;
    bool request_allowed_cached(AsyncWebServerRequest *request) const;
    void publish_pending_auth_config();

    // snapshot masks
    static constexpr uint16_t SNAPSHOT_STATUS = 1u << 0;
    static constexpr uint16_t SNAPSHOT_EXPORTS = 1u << 1;
    static constexpr uint16_t SNAPSHOT_CONFIG = 1u << 2;
    static constexpr uint16_t SNAPSHOT_AS11_BLE = 1u << 3;
    static constexpr uint16_t SNAPSHOT_OXIMETRY = 1u << 4;
    static constexpr uint16_t SNAPSHOT_SETTINGS = 1u << 5;
    static constexpr uint16_t SNAPSHOT_OTA = 1u << 6;
    static constexpr uint16_t SNAPSHOT_RESMED_OTA = 1u << 7;
    static constexpr uint16_t SNAPSHOT_RESMED_REPOSITORY = 1u << 8;
    static constexpr uint16_t SNAPSHOT_STORAGE_OPERATION = 1u << 9;
    static constexpr uint16_t SNAPSHOT_REPORT = 1u << 10;
    static constexpr uint16_t SNAPSHOT_ALL =
        SNAPSHOT_STATUS | SNAPSHOT_EXPORTS | SNAPSHOT_AS11_BLE |
        SNAPSHOT_OXIMETRY | SNAPSHOT_OTA | SNAPSHOT_RESMED_OTA |
        SNAPSHOT_RESMED_REPOSITORY | SNAPSHOT_STORAGE_OPERATION |
        SNAPSHOT_REPORT;
    static constexpr uint16_t SNAPSHOT_PERIODIC =
        SNAPSHOT_STATUS | SNAPSHOT_EXPORTS;

    enum class SnapshotChannelId : uint8_t {
        Config,
        As11Ble,
        Oximetry,
        Settings,
        Ota,
        ResmedOta,
        ResmedRepository,
        StorageOperation,
        Report,
        Count,
    };

    struct SnapshotChannel {
        const PublishedJsonSnapshot *source = nullptr;
        const char *event = nullptr;
        const char *checkpoint = nullptr;
        uint16_t mask = 0;
        size_t reserve = 0;
        LargeTextBuffer cached;
        LargeTextBuffer next;
        uint32_t observed_revision = 0;
    };

    static constexpr size_t SnapshotChannelCount =
        static_cast<size_t>(SnapshotChannelId::Count);
    SnapshotChannel &snapshot_channel(SnapshotChannelId id) {
        return snapshot_channels_[static_cast<size_t>(id)];
    }
    const SnapshotChannel &snapshot_channel(SnapshotChannelId id) const {
        return snapshot_channels_[static_cast<size_t>(id)];
    }

    // client tracking
    struct SseClientRef {
        AsyncEventSourceClient *client = nullptr;
        uint32_t connected_ms = 0;
        uint32_t last_status_ms = 0;
        uint32_t sent_snapshot_revisions[SnapshotChannelCount] = {};
    };

    // subsystem owners
    StatusHttpController *status_ = nullptr;
    ExportHttpController *exports_ = nullptr;
    LiveHttpController *live_ = nullptr;
    ConsoleCommandRouter *console_router_ = nullptr;

    // console and command queue
    ManagementConsole web_console_;
    char *console_log_ = nullptr;
    size_t console_log_capacity_ = 0;
    size_t console_log_start_ = 0;
    size_t console_log_length_ = 0;
    uint64_t console_log_write_pos_ = 0;
    uint32_t console_seq_ = 0;
    uint32_t console_sse_seq_ = 0;
    uint64_t console_sse_pos_ = 0;
    uint32_t last_console_sse_ms_ = 0;
    bool console_sse_reset_pending_ = false;
    FixedQueue<WebCommand, AC_WEB_COMMAND_QUEUE_DEPTH> command_queue_;

    // synchronization
    StaticSemaphore_t command_mutex_storage_ = {};
    StaticSemaphore_t cache_mutex_storage_ = {};
    StaticSemaphore_t sse_mutex_storage_ = {};
    SemaphoreHandle_t command_mutex_ = nullptr;
    SemaphoreHandle_t cache_mutex_ = nullptr;
    SemaphoreHandle_t sse_mutex_ = nullptr;

    // HTTP/SSE server
    AsyncWebServer *server_ = nullptr;
    AsyncEventSource *events_ = nullptr;
    SseClientRef sse_clients_[AC_WEB_SSE_CLIENTS_MAX + 1];
    bool sse_enforce_needed_ = false;

    // cached JSON snapshots
    LargeTextBuffer cached_status_json_;
    LargeTextBuffer next_status_json_;
    LargeTextBuffer cached_exports_json_;
    LargeTextBuffer next_exports_json_;
    SnapshotChannel snapshot_channels_[SnapshotChannelCount];

    // cached auth config
    bool cached_http_auth_required_ = true;
    String cached_http_user_;
    String cached_http_password_;
    String cached_auth_whitelist_;
    bool pending_http_auth_required_ = true;
    String pending_http_user_;
    String pending_http_password_;
    String pending_auth_whitelist_;
    bool auth_config_pending_ = false;

    // snapshot state
    uint32_t observed_status_revision_ = 0;
    uint32_t observed_live_generation_ = 0;
    bool snapshots_ready_ = false;
    uint16_t snapshots_dirty_mask_ = SNAPSHOT_ALL;
    uint32_t last_snapshot_ms_ = 0;
    uint32_t last_sse_push_ms_ = 0;
    std::atomic<bool> sse_push_requested_{false};
    bool started_ = false;
};

}  // namespace aircannect
