#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "app_config.h"
#include "large_text_buffer.h"
#include "management_console.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "resmed_ota_manager.h"
#include "rpc_arbiter.h"
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

struct WebLiveSeriesBatch {
    float values[AC_WEB_LIVE_BATCH_SAMPLES_MAX] = {};
    uint8_t valid[AC_WEB_LIVE_BATCH_SAMPLES_MAX] = {};
    size_t count = 0;
};

class WebUI {
public:
    bool begin(RpcArbiter &arbiter,
               WifiManager &wifi_manager,
               TcpBridge &tcp_bridge,
               AppConfig &app_config,
               TimeSyncService &time_sync_service,
               OtaManager &ota_manager,
               ResmedOtaManager &resmed_ota_manager,
               SessionManager &session_manager,
               SinkManager &sink_manager,
               OximetryManager &oximetry_manager,
               ConsoleContext &console_ctx,
               uint16_t port = 80);
    void stop();
    void poll();
    void handle_event(const RpcEvent &event);
    bool started() const { return started_; }

private:
    // Server setup and cached snapshots
    void register_routes();
    void reserve_cached_json();
    void build_status_json(LargeTextBuffer &json) const;
    void build_oximetry_sensors_json(LargeTextBuffer &json) const;
    void build_stream_json(LargeTextBuffer &json) const;
    void build_config_json(LargeTextBuffer &json) const;
    void build_wifi_json(LargeTextBuffer &json) const;
    void build_settings_json(LargeTextBuffer &json,
                             int requested_mode,
                             bool refresh_queued) const;
    void append_console_log(const String &text);
    void build_console_json(LargeTextBuffer &json) const;
    void send_cached_settings(AsyncWebServerRequest *request,
                              int requested_mode);
    void publish_snapshots(bool force);

    // Deferred command queue
    bool enqueue_command(struct WebCommand *command);
    bool enqueue_simple_command(uint8_t kind);
    void drain_commands();
    void execute_command(struct WebCommand &command);
    void execute_console_line(const std::string &line);
    void execute_config_update(const std::string &body);
    void execute_wifi_update(const std::string &body);
    void execute_time_action(const std::string &action);
    void execute_settings_update(const std::string &body);
    void execute_therapy_action(const std::string &action);
    void execute_oximetry_action(const std::string &action,
                                 const std::string &body);
    void execute_resmed_ota_command(const struct WebCommand &command);

    // SSE client tracking
    void handle_sse_connect(AsyncEventSourceClient *client);
    void handle_sse_disconnect(AsyncEventSourceClient *client);
    void enforce_sse_limits();
    size_t sse_client_count();

    // Dashboard live stream sink
    void poll_live_stream();
    void attach_live_stream(uint32_t now_ms);
    void release_live_stream();
    void drain_live_stream(uint32_t now_ms);
    void send_live_batch(uint32_t now_ms);
    void reset_live_batch();
    bool live_stream_should_run(size_t clients) const;
    bool live_stream_active() const;

    // Response helpers
    String queued_json(const char *result = "queued") const;
    void send_cached(AsyncWebServerRequest *request,
                     const LargeTextBuffer &json) const;
    void send_queue_result(AsyncWebServerRequest *request,
                           bool queued,
                           const char *result = "queued") const;
    bool request_allowed_cached(AsyncWebServerRequest *request) const;

    struct SseClientRef {
        AsyncEventSourceClient *client = nullptr;
        uint32_t connected_ms = 0;
    };

    RpcArbiter *arbiter_ = nullptr;
    WifiManager *wifi_manager_ = nullptr;
    TcpBridge *tcp_bridge_ = nullptr;
    AppConfig *app_config_ = nullptr;
    TimeSyncService *time_sync_service_ = nullptr;
    OtaManager *ota_manager_ = nullptr;
    ResmedOtaManager *resmed_ota_manager_ = nullptr;
    SessionManager *session_manager_ = nullptr;
    SinkManager *sink_manager_ = nullptr;
    OximetryManager *oximetry_manager_ = nullptr;
    ConsoleContext *console_ctx_ = nullptr;

    ManagementConsole web_console_;
    String console_log_;
    uint32_t console_seq_ = 0;
    uint32_t console_sse_seq_ = 0;
    QueueHandle_t command_queue_ = nullptr;
    SemaphoreHandle_t cache_mutex_ = nullptr;
    SemaphoreHandle_t sse_mutex_ = nullptr;

    AsyncWebServer *server_ = nullptr;
    AsyncEventSource *events_ = nullptr;
    SseClientRef sse_clients_[AC_WEB_SSE_CLIENTS_MAX + 1];
    bool sse_enforce_needed_ = false;

    StreamConsumerHandle live_stream_handle_ = STREAM_CONSUMER_INVALID;
    uint32_t live_next_attach_ms_ = 0;
    uint32_t live_last_send_ms_ = 0;
    uint32_t live_seq_ = 0;
    uint32_t live_frames_ = 0;
    uint32_t live_drops_ = 0;
    uint32_t live_attach_failures_ = 0;
    uint32_t live_last_frame_ms_ = 0;
    bool live_state_dirty_ = true;
    char live_last_error_[64] = {};
    WebLiveSeriesBatch live_pressure_;
    WebLiveSeriesBatch live_flow_;
    WebLiveSeriesBatch live_leak_;
    WebLiveSeriesBatch live_inspiratory_pressure_;
    WebLiveSeriesBatch live_expiratory_pressure_;
    WebLiveSeriesBatch live_spo2_;
    WebLiveSeriesBatch live_pulse_;

    LargeTextBuffer cached_status_json_;
    LargeTextBuffer cached_stream_json_;
    LargeTextBuffer cached_console_json_;
    LargeTextBuffer cached_config_json_;
    LargeTextBuffer cached_wifi_json_;
    LargeTextBuffer cached_oximetry_sensors_json_;
    LargeTextBuffer cached_ota_json_;
    LargeTextBuffer cached_resmed_ota_json_;
    LargeTextBuffer cached_settings_json_;
    LargeTextBuffer live_json_;

    bool cached_http_auth_required_ = true;
    String cached_http_user_;
    String cached_http_password_;
    String cached_auth_whitelist_;

    int cached_settings_mode_ = -1;
    int requested_settings_mode_ = -1;
    bool cached_settings_refresh_queued_ = false;
    bool snapshots_ready_ = false;
    bool snapshots_dirty_ = true;
    uint32_t last_snapshot_ms_ = 0;
    uint32_t last_sse_push_ms_ = 0;
    bool started_ = false;
};

}  // namespace aircannect
