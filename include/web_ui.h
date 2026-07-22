#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "app_config.h"
#include "fixed_queue.h"
#include "large_text_buffer.h"
#include "management_console.h"
#include "sink_manager.h"

class AsyncWebServerRequest;
class AsyncEventSourceClient;
class AsyncEventSource;
class AsyncWebServer;

namespace aircannect {

class HttpRouteModule;
class StatusHttpController;

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
               StreamBroker &stream,
               SinkManager &sink_manager,
               ConsoleContext &console_ctx,
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
    void reserve_cached_json();
    void send_live_view_state(AsyncWebServerRequest *request);
    void build_stream_json(LargeTextBuffer &json) const;
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
    void publish_pending_auth_config();

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
    static constexpr uint16_t SNAPSHOT_ALL =
        SNAPSHOT_STATUS | SNAPSHOT_STREAM;
    static constexpr uint16_t SNAPSHOT_PERIODIC =
        SNAPSHOT_STATUS | SNAPSHOT_STREAM;

    // subsystem owners
    StatusHttpController *status_ = nullptr;
    StreamBroker *stream_ = nullptr;
    SinkManager *sink_manager_ = nullptr;
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
    LargeTextBuffer next_status_json_;
    LargeTextBuffer next_stream_json_;
    LargeTextBuffer live_json_;

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
    bool snapshots_ready_ = false;
    uint16_t snapshots_dirty_mask_ = SNAPSHOT_ALL;
    uint32_t last_snapshot_ms_ = 0;
    uint32_t last_sse_push_ms_ = 0;
    bool sse_push_requested_ = false;
    bool started_ = false;
};

}  // namespace aircannect
