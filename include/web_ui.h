#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "as11_device_service.h"
#include "config_service.h"
#include "fixed_queue.h"
#include "large_text_buffer.h"
#include "management_console.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "rpc_request_port.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "tcp_bridge.h"
#include "time_sync_service.h"
#include "wifi_manager.h"

class AsyncWebServerRequest;
class AsyncEventSourceClient;
class AsyncEventSource;
class AsyncWebServer;

namespace aircannect {

class HttpRouteModule;

enum WebCommandKind : uint8_t {
    WebCommandConsoleLine,
    WebCommandConsoleClear,
    WebCommandConfigUpdate,
    WebCommandWifiUpdate,
    WebCommandTimeAction,
    WebCommandTherapyAction,
    WebCommandOximetryAction,
};

struct WebCommand {
    uint8_t kind = WebCommandConsoleLine;
    std::string text;
    std::string body;
};

struct WebUiBufferMemoryStatus {
    size_t length = 0;
    size_t capacity = 0;
};

struct WebUiMemoryStatus {
    bool started = false;
    WebUiBufferMemoryStatus status;
    WebUiBufferMemoryStatus stream;
    WebUiBufferMemoryStatus console;
    WebUiBufferMemoryStatus config;
    WebUiBufferMemoryStatus wifi;
    WebUiBufferMemoryStatus oximetry_sensors;
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
    bool begin(RpcRequestPort &rpc,
               StreamBroker &stream,
               As11DeviceService &device,
               WifiManager &wifi_manager,
               TcpBridge &tcp_bridge,
               ConfigService &config_service,
               TimeSyncService &time_sync_service,
               OtaManager &ota_manager,
               SessionManager &session_manager,
               SinkManager &sink_manager,
               OximetryManager &oximetry_manager,
               ConsoleContext &console_ctx,
               HttpRouteModule *const *route_modules,
               size_t route_module_count,
               uint16_t port = 80);
    void stop();
    void poll(PollCheckpoint checkpoint = nullptr);

    // inbound device events
    void handle_event(const RpcEvent &event);

    // status
    bool started() const { return started_; }
    WebUiMemoryStatus memory_status();

private:
    // Server setup and cached snapshots
    void register_routes(HttpRouteModule *const *route_modules,
                         size_t route_module_count);
    void reserve_cached_json();
    void build_status_json(LargeTextBuffer &json,
                           PollCheckpoint checkpoint = nullptr) const;
    void build_oximetry_sensors_json(LargeTextBuffer &json) const;
    void send_live_view_state(AsyncWebServerRequest *request);
    void build_stream_json(LargeTextBuffer &json) const;
    void build_config_json(LargeTextBuffer &json,
                           const char *section = nullptr) const;
    void build_config_schema_json(LargeTextBuffer &json) const;
    void send_config_json(AsyncWebServerRequest *request,
                          const char *section = nullptr) const;
    void send_config_schema_json(AsyncWebServerRequest *request) const;
    void send_config_update(AsyncWebServerRequest *request);
    void build_wifi_json(LargeTextBuffer &json) const;
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
                           bool realtime_active = false,
                           PollCheckpoint checkpoint = nullptr);

    // Deferred command queue
    bool enqueue_command(WebCommand &&command);
    bool enqueue_simple_command(uint8_t kind);
    void drain_commands();
    void execute_command(WebCommand &command);
    void execute_console_line(const std::string &line);
    void execute_config_update(const std::string &body);
    void execute_wifi_update(const std::string &body);
    void execute_time_action(const std::string &action);
    void execute_therapy_action(const std::string &action);
    void execute_oximetry_action(const std::string &action,
                                 const std::string &body);

    // SSE client tracking
    void handle_sse_connect(AsyncEventSourceClient *client);
    void handle_sse_disconnect(AsyncEventSourceClient *client);
    void enforce_sse_limits();
    size_t healthy_sse_client_count();
    enum class SseSendResult : uint8_t { Skipped, Sent, Failed };
    SseSendResult send_sse_to_clients(const char *payload, const char *event,
                                      uint32_t id, bool status_heartbeat);

    // Dashboard live stream sink
    void poll_live_stream();
    void send_live_batch(uint32_t now_ms);
    bool live_view_requested(uint32_t now_ms);

    // Response helpers
    String queued_json(const char *result = "queued") const;
    void send_cached(AsyncWebServerRequest *request,
                     const LargeTextBuffer &json) const;
    void send_queue_result(AsyncWebServerRequest *request,
                           bool queued,
                           const char *result = "queued") const;
    bool request_allowed_cached(AsyncWebServerRequest *request) const;

    // client tracking
    struct SseClientRef {
        AsyncEventSourceClient *client = nullptr;
        uint32_t connected_ms = 0;
        uint32_t last_status_ms = 0;
    };

    // live view leases
    struct LiveViewLease {
        uint32_t client_hash = 0;
        uint32_t expires_ms = 0;
    };

    // snapshot masks
    static constexpr uint16_t SNAPSHOT_STATUS = 1u << 0;
    static constexpr uint16_t SNAPSHOT_STREAM = 1u << 1;
    static constexpr uint16_t SNAPSHOT_CONFIG = 1u << 3;
    static constexpr uint16_t SNAPSHOT_WIFI = 1u << 4;
    static constexpr uint16_t SNAPSHOT_OXIMETRY_SENSORS = 1u << 5;
    static constexpr uint16_t SNAPSHOT_ALL =
        SNAPSHOT_STATUS | SNAPSHOT_STREAM | SNAPSHOT_CONFIG |
        SNAPSHOT_WIFI | SNAPSHOT_OXIMETRY_SENSORS;
    static constexpr uint16_t SNAPSHOT_PERIODIC =
        SNAPSHOT_STATUS | SNAPSHOT_STREAM | SNAPSHOT_WIFI |
        SNAPSHOT_OXIMETRY_SENSORS;

    // subsystem owners
    RpcRequestPort *rpc_ = nullptr;
    StreamBroker *stream_ = nullptr;
    As11DeviceService *device_ = nullptr;
    WifiManager *wifi_manager_ = nullptr;
    TcpBridge *tcp_bridge_ = nullptr;
    ConfigService *config_service_ = nullptr;
    TimeSyncService *time_sync_service_ = nullptr;
    OtaManager *ota_manager_ = nullptr;
    SessionManager *session_manager_ = nullptr;
    SinkManager *sink_manager_ = nullptr;
    OximetryManager *oximetry_manager_ = nullptr;
    ConsoleContext *console_ctx_ = nullptr;

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
    bool console_sse_reset_pending_ = false;
    FixedQueue<WebCommand, AC_WEB_COMMAND_QUEUE_DEPTH> command_queue_;

    // synchronization
    StaticSemaphore_t command_mutex_storage_ = {};
    StaticSemaphore_t cache_mutex_storage_ = {};
    StaticSemaphore_t sse_mutex_storage_ = {};
    StaticSemaphore_t live_view_mutex_storage_ = {};
    SemaphoreHandle_t command_mutex_ = nullptr;
    SemaphoreHandle_t cache_mutex_ = nullptr;
    SemaphoreHandle_t sse_mutex_ = nullptr;
    SemaphoreHandle_t live_view_mutex_ = nullptr;

    // HTTP/SSE server
    AsyncWebServer *server_ = nullptr;
    AsyncEventSource *events_ = nullptr;
    SseClientRef sse_clients_[AC_WEB_SSE_CLIENTS_MAX + 1];
    bool sse_enforce_needed_ = false;

    // dashboard live stream
    uint32_t live_last_send_ms_ = 0;
    uint32_t live_seq_ = 0;
    LiveViewLease live_view_leases_[AC_WEB_SSE_CLIENTS_MAX + 1];

    // cached JSON snapshots
    LargeTextBuffer cached_status_json_;
    LargeTextBuffer cached_stream_json_;
    LargeTextBuffer cached_wifi_json_;
    LargeTextBuffer cached_oximetry_sensors_json_;
    LargeTextBuffer live_json_;

    // cached auth config
    bool cached_http_auth_required_ = true;
    String cached_http_user_;
    String cached_http_password_;
    String cached_auth_whitelist_;

    // snapshot state
    uint32_t observed_device_revision_ = 0;
    uint32_t observed_config_revision_ = 0;
    bool snapshots_ready_ = false;
    uint16_t snapshots_dirty_mask_ = SNAPSHOT_ALL;
    uint32_t last_snapshot_ms_ = 0;
    uint32_t last_sse_push_ms_ = 0;
    bool sse_push_requested_ = false;
    bool started_ = false;
};

}  // namespace aircannect
