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
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "report_manager.h"
#include "resmed_ota_manager.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "sleephq_sync_job.h"
#include "storage_archive_job.h"
#include "storage_delete_job.h"
#include "storage_sync_job.h"
#include "tcp_bridge.h"
#include "time_sync_service.h"
#include "wifi_manager.h"

class AsyncWebServerRequest;
class AsyncEventSourceClient;
class AsyncEventSource;
class AsyncWebServer;

namespace aircannect {

class ExportCoordinator;

enum WebCommandKind : uint8_t {
    WebCommandConsoleLine,
    WebCommandConsoleClear,
    WebCommandConfigUpdate,
    WebCommandWifiUpdate,
    WebCommandTimeAction,
    WebCommandSettingsRefresh,
    WebCommandSettingsUpdate,
    WebCommandTherapyAction,
    WebCommandOximetryAction,
    WebCommandReportSummaryRefresh,
    WebCommandResmedOtaInit,
    WebCommandResmedOtaBlock,
    WebCommandResmedOtaCheck,
    WebCommandResmedOtaApply,
    WebCommandResmedOtaAbort,
    WebCommandResmedOtaStartStaged,
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
    WebUiBufferMemoryStatus ota;
    WebUiBufferMemoryStatus resmed_ota;
    WebUiBufferMemoryStatus settings;
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
               ReportManager &report_manager,
               StorageArchiveJob &storage_archive_job,
               StorageDeleteJob &storage_delete_job,
               ExportCoordinator &export_coordinator,
               StorageSyncJob *storage_sync_job,
               SleepHqSyncJob *sleephq_sync_job,
               ConsoleContext &console_ctx,
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
    void register_routes();
    void reserve_cached_json();
    void build_status_json(LargeTextBuffer &json,
                           PollCheckpoint checkpoint = nullptr) const;
    void build_oximetry_sensors_json(LargeTextBuffer &json) const;
    void send_report_summary(AsyncWebServerRequest *request) const;
    void send_report_chunks(AsyncWebServerRequest *request) const;
    void send_report_plot(AsyncWebServerRequest *request) const;
    void send_report_result(AsyncWebServerRequest *request) const;
    void send_storage_list(AsyncWebServerRequest *request) const;
    void send_storage_download(AsyncWebServerRequest *request) const;
    void send_storage_archive_start(AsyncWebServerRequest *request) const;
    void send_storage_archive_status(AsyncWebServerRequest *request) const;
    void send_storage_archive_download(AsyncWebServerRequest *request) const;
    void send_storage_delete_start(AsyncWebServerRequest *request) const;
    void send_storage_delete_status(AsyncWebServerRequest *request) const;
    void send_storage_sync_start(AsyncWebServerRequest *request) const;
    void send_storage_sync_verify(AsyncWebServerRequest *request) const;
    void send_storage_sync_status(AsyncWebServerRequest *request) const;
    void send_sleephq_sync_start(AsyncWebServerRequest *request) const;
    void send_sleephq_sync_check(AsyncWebServerRequest *request) const;
    void send_sleephq_sync_status(AsyncWebServerRequest *request) const;
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
    void build_settings_json(LargeTextBuffer &json,
                             int requested_mode,
                             bool refresh_queued) const;
    void build_settings_catalog_json(LargeTextBuffer &json) const;
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
    void send_cached_settings(AsyncWebServerRequest *request,
                              int requested_mode);
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
    void execute_settings_update(const std::string &body);
    void execute_therapy_action(const std::string &action);
    void execute_oximetry_action(const std::string &action,
                                 const std::string &body);
    void execute_report_summary_refresh();
    void execute_resmed_ota_command(const WebCommand &command);

    // SSE client tracking
    void handle_sse_connect(AsyncEventSourceClient *client);
    void handle_sse_disconnect(AsyncEventSourceClient *client);
    void enforce_sse_limits();
    size_t sse_client_count();
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
    static constexpr uint16_t SNAPSHOT_OTA = 1u << 6;
    static constexpr uint16_t SNAPSHOT_RESMED_OTA = 1u << 7;
    static constexpr uint16_t SNAPSHOT_SETTINGS = 1u << 8;
    static constexpr uint16_t SNAPSHOT_ALL =
        SNAPSHOT_STATUS | SNAPSHOT_STREAM | SNAPSHOT_CONFIG |
        SNAPSHOT_WIFI | SNAPSHOT_OXIMETRY_SENSORS | SNAPSHOT_OTA |
        SNAPSHOT_RESMED_OTA | SNAPSHOT_SETTINGS;
    static constexpr uint16_t SNAPSHOT_PERIODIC =
        SNAPSHOT_STATUS | SNAPSHOT_STREAM | SNAPSHOT_WIFI |
        SNAPSHOT_OXIMETRY_SENSORS | SNAPSHOT_OTA |
        SNAPSHOT_RESMED_OTA;

    // subsystem owners
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
    ReportManager *report_manager_ = nullptr;
    StorageArchiveJob *storage_archive_job_ = nullptr;
    StorageDeleteJob *storage_delete_job_ = nullptr;
    ExportCoordinator *export_coordinator_ = nullptr;
    StorageSyncJob *storage_sync_job_ = nullptr;
    SleepHqSyncJob *sleephq_sync_job_ = nullptr;
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
    StaticSemaphore_t storage_job_mutex_storage_ = {};
    SemaphoreHandle_t command_mutex_ = nullptr;
    SemaphoreHandle_t cache_mutex_ = nullptr;
    SemaphoreHandle_t sse_mutex_ = nullptr;
    SemaphoreHandle_t live_view_mutex_ = nullptr;
    SemaphoreHandle_t storage_job_mutex_ = nullptr;

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
    LargeTextBuffer cached_ota_json_;
    LargeTextBuffer cached_resmed_ota_json_;
    LargeTextBuffer cached_settings_json_;
    LargeTextBuffer live_json_;

    // cached auth config
    bool cached_http_auth_required_ = true;
    String cached_http_user_;
    String cached_http_password_;
    String cached_auth_whitelist_;

    // snapshot state
    int cached_settings_mode_ = -1;
    int requested_settings_mode_ = -1;
    bool cached_settings_refresh_queued_ = false;
    bool snapshots_ready_ = false;
    uint16_t snapshots_dirty_mask_ = SNAPSHOT_ALL;
    uint32_t last_snapshot_ms_ = 0;
    uint32_t last_sse_push_ms_ = 0;
    bool sse_push_requested_ = false;
    bool started_ = false;
};

}  // namespace aircannect
