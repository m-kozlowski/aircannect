#include "web_ui.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <string.h>
#include <utility>

#include "auth_utils.h"
#include "board.h"
#include "config_http_controller.h"
#include "debug_log.h"
#include "device_http_controller.h"
#include "export_http_controller.h"
#include "http_route_module.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "live_http_controller.h"
#include "memory_manager.h"
#include "oximetry_http_controller.h"
#include "ota_http_controller.h"
#include "report_http_controller.h"
#include "resmed_firmware_http_controller.h"
#include "rpc_transport_ports.h"
#include "settings_http_controller.h"
#include "status_http_controller.h"
#include "storage_http_controller.h"
#include "string_util.h"
#include "string_print.h"
#include "web_ui_html.h"

namespace aircannect {

namespace {

static constexpr size_t WEB_EXPORT_STATUS_JSON_RESERVE = 3072;

const char *web_command_name(uint8_t kind) {
    switch (kind) {
        case WebCommandConsoleLine: return "console_line";
        case WebCommandConsoleClear: return "console_clear";
        default: return "unknown";
    }
}

size_t json_escaped_capacity(size_t raw_len, size_t overhead = 128) {
    // JSON string escaping can expand a control byte to "\\u00XX".
    return overhead + raw_len * 6;
}

void send_web_ui(AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse(
        200, "text/html", HTML_PAGE_GZ, HTML_PAGE_GZ_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

}  // namespace

bool WebUI::begin(StatusHttpController &status,
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
                  uint16_t port) {
    if (started_) return true;
    stop();
    status_ = &status;
    exports_ = &exports;
    live_ = &live;
    console_router_ = &console_router;
    bind_snapshot_channels(config_http, device_http, oximetry_http,
                           settings_http, ota_http,
                           resmed_firmware_http, storage_http,
                           report_http);

    if (!command_mutex_) {
        command_mutex_ = xSemaphoreCreateMutexStatic(&command_mutex_storage_);
    }
    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!sse_mutex_) {
        sse_mutex_ = xSemaphoreCreateMutexStatic(&sse_mutex_storage_);
    }
    if (!command_mutex_ || !cache_mutex_ || !sse_mutex_) {
        stop();
        return false;
    }
    reserve_cached_json();
    apply_auth_config(config);

    server_ = new AsyncWebServer(port);
    if (!server_) {
        stop();
        return false;
    }
    server_->addMiddleware([this](AsyncWebServerRequest *request,
                                 ArMiddlewareNext next) {
        if (request_allowed_cached(request)) {
            next();
            return;
        }
        request->requestAuthentication(AsyncAuthType::AUTH_BASIC,
                                       "AirCANnect");
    });
    events_ = new AsyncEventSource("/api/events");
    if (events_) {
        events_->onConnect([this](AsyncEventSourceClient *client) {
            handle_sse_connect(client);
        });
        events_->onDisconnect([this](AsyncEventSourceClient *client) {
            handle_sse_disconnect(client);
        });
        server_->addHandler(events_);
    }
    register_routes(route_modules, route_module_count);
    publish_snapshots(true);
    server_->begin();
    started_ = true;
    Log::logf(CAT_GENERAL, LOG_INFO, "[WEB] listening on port %u\n", port);
    return true;
}

void WebUI::bind_snapshot_channels(
    ConfigHttpController &config_http,
    DeviceHttpController &device_http,
    OximetryHttpController &oximetry_http,
    SettingsHttpController &settings_http,
    OtaHttpController &ota_http,
    ResmedFirmwareHttpController &resmed_firmware_http,
    StorageHttpController &storage_http,
    ReportHttpController &report_http) {
    auto bind = [this](SnapshotChannelId id,
                       const PublishedJsonSnapshot &source,
                       const char *event,
                       const char *checkpoint,
                       uint16_t mask,
                       size_t reserve) {
        SnapshotChannel &channel = snapshot_channel(id);
        channel.source = &source;
        channel.event = event;
        channel.checkpoint = checkpoint;
        channel.mask = mask;
        channel.reserve = reserve;
    };

    bind(SnapshotChannelId::Config, config_http.update_snapshot(),
         "config", "web_ui.snapshots.config_copy", SNAPSHOT_CONFIG, 256);
    bind(SnapshotChannelId::As11Ble, device_http.ble_pairing_snapshot(),
         "as11_ble", "web_ui.snapshots.as11_ble_copy",
         SNAPSHOT_AS11_BLE, AC_WEB_AS11_BLE_STATUS_JSON_RESERVE);
    bind(SnapshotChannelId::Oximetry, oximetry_http.snapshot(),
         "oximetry", "web_ui.snapshots.oximetry_copy",
         SNAPSHOT_OXIMETRY, AC_WEB_OXIMETRY_SENSORS_JSON_RESERVE);
    bind(SnapshotChannelId::Settings, settings_http.snapshot(),
         "settings", "web_ui.snapshots.settings_copy",
         SNAPSHOT_SETTINGS, AC_WEB_SETTINGS_JSON_RESERVE);
    bind(SnapshotChannelId::Ota, ota_http.snapshot(),
         "ota", "web_ui.snapshots.ota_copy",
         SNAPSHOT_OTA, AC_WEB_OTA_JSON_RESERVE);
    bind(SnapshotChannelId::ResmedOta, ota_http.resmed_snapshot(),
         "resmed_ota", "web_ui.snapshots.resmed_ota_copy",
         SNAPSHOT_RESMED_OTA, AC_WEB_RESMED_OTA_JSON_RESERVE);
    bind(SnapshotChannelId::ResmedRepository,
         resmed_firmware_http.status_snapshot(),
         "resmed_repository", "web_ui.snapshots.resmed_repository_copy",
         SNAPSHOT_RESMED_REPOSITORY,
         AC_WEB_RESMED_REPOSITORY_JSON_RESERVE);
    bind(SnapshotChannelId::StorageOperation,
         storage_http.operation_snapshot(),
         "storage_operation", "web_ui.snapshots.storage_operation_copy",
         SNAPSHOT_STORAGE_OPERATION,
         AC_WEB_STORAGE_OPERATION_JSON_RESERVE);
    bind(SnapshotChannelId::Report,
         report_http.completion_snapshot(),
         "report", "web_ui.snapshots.report_copy",
         SNAPSHOT_REPORT, 384);
}

void WebUI::reserve_cached_json() {
    cached_status_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    next_status_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    cached_exports_json_.reserve(WEB_EXPORT_STATUS_JSON_RESERVE);
    next_exports_json_.reserve(WEB_EXPORT_STATUS_JSON_RESERVE);

    for (SnapshotChannel &channel : snapshot_channels_) {
        channel.cached.reserve(channel.reserve);
        channel.next.reserve(channel.reserve);
    }
}

WebUiMemoryStatus WebUI::memory_status() {
    auto capture = [](const LargeTextBuffer &buffer) {
        WebUiBufferMemoryStatus out;
        out.length = buffer.length();
        out.capacity = buffer.capacity();
        return out;
    };

    WebUiMemoryStatus out;
    out.started = started_;
    if (sse_mutex_ && xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            AsyncEventSourceClient *client = ref.client;
            if (!client) continue;
            if (!client->connected()) {
                ref = {};
                continue;
            }
            const size_t pending = client->packetsWaiting();
            out.sse_clients++;
            out.sse_pending_total += pending;
            if (pending > out.sse_pending_worst) {
                out.sse_pending_worst = pending;
            }
        }
        xSemaphoreGive(sse_mutex_);
    } else {
        out.sse_clients = events_ ? events_->count() : 0;
    }
    if (live_) {
        const LiveHttpMemoryStatus live = live_->memory_status();
        out.stream.length = live.stream_length;
        out.stream.capacity = live.stream_capacity;
        out.live.length = live.live_length;
        out.live.capacity = live.live_capacity;
    }

    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return out;
    }
    out.status = capture(cached_status_json_);
    out.exports = capture(cached_exports_json_);
    out.config = capture(
        snapshot_channel(SnapshotChannelId::Config).cached);
    out.as11_ble = capture(
        snapshot_channel(SnapshotChannelId::As11Ble).cached);
    out.oximetry = capture(
        snapshot_channel(SnapshotChannelId::Oximetry).cached);
    out.settings = capture(
        snapshot_channel(SnapshotChannelId::Settings).cached);
    out.ota = capture(snapshot_channel(SnapshotChannelId::Ota).cached);
    out.resmed_ota = capture(
        snapshot_channel(SnapshotChannelId::ResmedOta).cached);
    out.resmed_repository = capture(
        snapshot_channel(SnapshotChannelId::ResmedRepository).cached);
    out.storage_operation = capture(
        snapshot_channel(SnapshotChannelId::StorageOperation).cached);
    out.report = capture(
        snapshot_channel(SnapshotChannelId::Report).cached);
    out.console.length = console_log_length_;
    out.console.capacity = console_log_capacity_;
    out.console_log_length = console_log_length_;
    xSemaphoreGive(cache_mutex_);
    return out;
}

void WebUI::stop() {
    if (live_) live_->stop();
    if (console_router_) web_console_.stop(*console_router_);
    if (events_) {
        events_->close();
    }
    if (server_) {
        server_->end();
        delete server_;
        server_ = nullptr;
    } else if (events_) {
        delete events_;
    }
    events_ = nullptr;

    if (command_mutex_) {
        if (xSemaphoreTake(command_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            command_queue_.clear();
            xSemaphoreGive(command_mutex_);
        } else {
            command_queue_.clear();
        }
    }

    if (sse_mutex_) {
        if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (SseClientRef &ref : sse_clients_) ref = {};
            xSemaphoreGive(sse_mutex_);
        }
    } else {
        for (SseClientRef &ref : sse_clients_) ref = {};
    }

    sse_enforce_needed_ = false;
    Memory::free(console_log_);
    console_log_ = nullptr;
    console_log_capacity_ = 0;
    console_log_start_ = 0;
    console_log_length_ = 0;
    console_log_write_pos_ = 0;
    console_sse_pos_ = 0;
    last_console_sse_ms_ = 0;
    console_sse_reset_pending_ = false;
    snapshots_ready_ = false;
    snapshots_dirty_mask_ = SNAPSHOT_ALL;
    observed_status_revision_ = 0;
    for (SnapshotChannel &channel : snapshot_channels_) {
        channel.source = nullptr;
        channel.observed_revision = 0;
    }
    observed_live_generation_ = 0;
    last_snapshot_ms_ = 0;
    last_sse_push_ms_ = 0;
    sse_push_requested_.store(false, std::memory_order_release);
    status_ = nullptr;
    exports_ = nullptr;
    live_ = nullptr;
    console_router_ = nullptr;
    started_ = false;
}

void WebUI::poll(PollCheckpoint checkpoint) {
    if (!started_) return;
    publish_pending_auth_config();

    if (status_ && observed_status_revision_ != status_->revision()) {
        mark_snapshots_dirty(SNAPSHOT_STATUS);
    }
    for (const SnapshotChannel &channel : snapshot_channels_) {
        if (channel.source &&
            channel.observed_revision != channel.source->revision()) {
            mark_snapshots_dirty(channel.mask);
        }
    }

    if (console_router_ && web_console_.pending_output(*console_router_)) {
        StringPrint capture(AC_FILE_LOG_TAIL_READ_CHUNK);
        web_console_.poll_pending(capture, *console_router_);
        if (capture.text().length()) append_console_log(capture.text());
    }

    drain_commands();
    if (checkpoint) checkpoint("web_ui.commands");
    enforce_sse_limits();
    if (checkpoint) checkpoint("web_ui.sse_limits");

    size_t connected_sse_clients = 0;
    size_t healthy_sse_clients = 0;
    count_sse_clients(connected_sse_clients, healthy_sse_clients);
    poll_live_transport(connected_sse_clients, healthy_sse_clients);
    if (checkpoint) checkpoint("web_ui.live");

    publish_snapshots(false, checkpoint);
    if (checkpoint) checkpoint("web_ui.snapshots");

    if (!events_ || events_->count() == 0) return;

    const uint32_t now_ms = millis();
    const bool status_push_due =
        sse_push_requested_.load(std::memory_order_acquire) ||
        static_cast<int32_t>(now_ms - last_sse_push_ms_) >=
            static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    const bool console_push_due =
        console_sse_seq_ != console_seq_ &&
        static_cast<int32_t>(now_ms - last_console_sse_ms_) >=
            static_cast<int32_t>(AC_WEB_CONSOLE_PUSH_INTERVAL_MS);
    if (!status_push_due && !console_push_due) {
        if (checkpoint) checkpoint("web_ui.sse_idle");
        return;
    }
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        if (checkpoint) checkpoint("web_ui.sse_lock");
        return;
    }

    const uint32_t event_id = now_ms;
    bool sse_backpressure = false;

    if (status_push_due) {
        last_sse_push_ms_ = now_ms;
        (void)sse_push_requested_.exchange(false,
                                           std::memory_order_acq_rel);

        if (send_sse_to_clients(cached_status_json_.c_str(), "status",
                                event_id, true) == SseSendResult::Failed) {
            sse_backpressure = true;
        }

        if (cached_exports_json_.length() &&
            send_sse_to_clients(cached_exports_json_.c_str(), "exports",
                                event_id, false) == SseSendResult::Failed) {
            sse_backpressure = true;
        }

        for (size_t i = 0; i < SnapshotChannelCount; ++i) {
            const SseSendResult result =
                send_snapshot_to_clients(i, event_id);
            if (result == SseSendResult::Failed) {
                sse_backpressure = true;
            }
        }

        const char *stream_payload = nullptr;
        size_t stream_length = 0;
        if (live_ && live_->stream_payload(stream_payload, stream_length) &&
            stream_length &&
            send_sse_to_clients(stream_payload, "stream", event_id, false) ==
                SseSendResult::Failed) {
            sse_backpressure = true;
        }
    }

    if (console_push_due) {
        last_console_sse_ms_ = now_ms;
        const uint64_t begin = console_log_begin_pos();
        const uint64_t end = console_log_write_pos_;
        const bool reset = console_sse_reset_pending_ ||
                           console_sse_pos_ < begin ||
                           console_sse_pos_ > end;
        const uint64_t from = reset ? begin : console_sse_pos_;
        const size_t payload_len =
            static_cast<size_t>(end > from ? end - from : 0);
        LargeTextBuffer console_json;
        console_json.reserve(json_escaped_capacity(payload_len));
        build_console_sse_json(console_json);
        const SseSendResult console_result =
            console_json.overflowed()
                ? SseSendResult::Failed
                : send_sse_to_clients(console_json.c_str(), "console",
                                      event_id, false);
        if (console_result == SseSendResult::Failed) {
            sse_backpressure = true;
        } else if (console_result == SseSendResult::Sent) {
            note_console_sse_sent();
        }
    }
    xSemaphoreGive(cache_mutex_);
    if (checkpoint) checkpoint("web_ui.sse_send");
    if (sse_backpressure) {
        sse_enforce_needed_ = true;
        enforce_sse_limits();
        if (checkpoint) checkpoint("web_ui.sse_backpressure");
    }
}

void WebUI::apply_auth_config(const AppConfigData &config) {
    pending_http_auth_required_ = network_auth_required(config);
    pending_http_user_ = config.http_user;
    pending_http_password_ = config.http_password;
    pending_auth_whitelist_ = config.auth_whitelist;
    auth_config_pending_ = true;
    publish_pending_auth_config();
}

void WebUI::publish_pending_auth_config() {
    if (!auth_config_pending_ || !cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        return;
    }

    cached_http_auth_required_ = pending_http_auth_required_;
    cached_http_user_ = pending_http_user_;
    cached_http_password_ = pending_http_password_;
    cached_auth_whitelist_ = pending_auth_whitelist_;
    auth_config_pending_ = false;
    xSemaphoreGive(cache_mutex_);
}

void WebUI::handle_sse_connect(AsyncEventSourceClient *client) {
    if (!client || !sse_mutex_) return;

    bool stored = false;
    if (xSemaphoreTake(sse_mutex_, portMAX_DELAY) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client && !ref.client->connected()) {
                ref = {};
            }
        }
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client == client) {
                stored = true;
                break;
            }
        }
        if (!stored) {
            for (SseClientRef &ref : sse_clients_) {
                if (ref.client) continue;
                ref = {};
                ref.client = client;
                ref.connected_ms = millis();
                stored = true;
                break;
            }
        }
        sse_enforce_needed_ = true;
        xSemaphoreGive(sse_mutex_);
    }

    if (!stored) {
        client->close();
        return;
    }

    request_sse_push();
}

void WebUI::handle_sse_disconnect(AsyncEventSourceClient *client) {
    if (!client || !sse_mutex_) return;

    if (xSemaphoreTake(sse_mutex_, portMAX_DELAY) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client != client) continue;
            ref = {};
            break;
        }
        xSemaphoreGive(sse_mutex_);
    }
}

void WebUI::enforce_sse_limits() {
    if (!sse_mutex_) return;

    while (true) {
        AsyncEventSourceClient *drop = nullptr;
        size_t connected_count = 0;
        size_t worst_pending = 0;
        uint32_t oldest_ms = 0;

        if (xSemaphoreTake(sse_mutex_, 0) != pdTRUE) return;
        for (SseClientRef &ref : sse_clients_) {
            AsyncEventSourceClient *client = ref.client;
            if (!client) continue;
            if (!client->connected()) {
                ref = {};
                continue;
            }

            connected_count++;
            const size_t pending = client->packetsWaiting();
            const bool worse_pending = pending > worst_pending;
            const bool older_tie =
                pending == worst_pending &&
                (!oldest_ms ||
                 static_cast<int32_t>(ref.connected_ms - oldest_ms) < 0);
            if (!drop || worse_pending || older_tie) {
                drop = client;
                worst_pending = pending;
                oldest_ms = ref.connected_ms;
            }
        }

        const bool overflow = connected_count > AC_WEB_SSE_CLIENTS_MAX;
        const bool slow = worst_pending > AC_WEB_SSE_CLIENT_PENDING_HARD_MAX;
        if (!overflow && !slow) {
            sse_enforce_needed_ = false;
            xSemaphoreGive(sse_mutex_);
            return;
        }
        xSemaphoreGive(sse_mutex_);

        if (!drop) return;
        Log::logf(CAT_GENERAL, LOG_DEBUG,
                  "[WEB] closing SSE client count=%u pending=%u\n",
                  static_cast<unsigned>(connected_count),
                  static_cast<unsigned>(worst_pending));
        drop->close();
    }
}

void WebUI::count_sse_clients(size_t &connected, size_t &healthy) {
    connected = 0;
    healthy = 0;
    if (!sse_mutex_) return;

    if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        connected = events_ ? events_->count() : 0;
        healthy = connected;
        return;
    }
    for (SseClientRef &ref : sse_clients_) {
        AsyncEventSourceClient *client = ref.client;
        if (!client) continue;
        if (!client->connected()) {
            ref = {};
            continue;
        }
        connected++;
        if (client->packetsWaiting() <= AC_WEB_SSE_CLIENT_PENDING_MAX) {
            healthy++;
        }
    }
    xSemaphoreGive(sse_mutex_);
}

WebUI::SseSendResult WebUI::send_sse_to_clients(const char *payload,
                                                const char *event,
                                                uint32_t id,
                                                bool status_heartbeat) {
    if (!payload || !event || !sse_mutex_) return SseSendResult::Skipped;

    const uint32_t now = millis();
    SseSendResult result = SseSendResult::Skipped;
    if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return SseSendResult::Failed;
    }

    for (SseClientRef &ref : sse_clients_) {
        AsyncEventSourceClient *client = ref.client;
        if (!client) continue;
        if (!client->connected()) {
            ref = {};
            continue;
        }

        const size_t pending = client->packetsWaiting();
        if (pending > AC_WEB_SSE_CLIENT_PENDING_MAX) {
            if (!status_heartbeat) continue;
            if (ref.last_status_ms &&
                static_cast<int32_t>(now - ref.last_status_ms) <
                    static_cast<int32_t>(AC_WEB_SSE_BACKPRESSURE_STATUS_MS)) {
                continue;
            }
        }

        if (!client->send(payload, event, id)) {
            result = SseSendResult::Failed;
            continue;
        }
        if (result != SseSendResult::Failed) result = SseSendResult::Sent;
        if (status_heartbeat) ref.last_status_ms = now;
    }

    xSemaphoreGive(sse_mutex_);
    return result;
}

WebUI::SseSendResult WebUI::send_snapshot_to_clients(
    size_t channel_index, uint32_t id) {
    if (channel_index >= SnapshotChannelCount || !sse_mutex_) {
        return SseSendResult::Skipped;
    }

    const SnapshotChannel &channel = snapshot_channels_[channel_index];
    if (!channel.cached.length() || !channel.event ||
        !channel.observed_revision) {
        return SseSendResult::Skipped;
    }
    if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return SseSendResult::Failed;
    }

    SseSendResult result = SseSendResult::Skipped;
    for (SseClientRef &ref : sse_clients_) {
        AsyncEventSourceClient *client = ref.client;
        if (!client) continue;
        if (!client->connected()) {
            ref = {};
            continue;
        }
        if (ref.sent_snapshot_revisions[channel_index] ==
            channel.observed_revision) {
            continue;
        }
        if (client->packetsWaiting() > AC_WEB_SSE_CLIENT_PENDING_MAX) {
            continue;
        }

        if (!client->send(channel.cached.c_str(), channel.event, id)) {
            result = SseSendResult::Failed;
            continue;
        }

        ref.sent_snapshot_revisions[channel_index] =
            channel.observed_revision;
        if (result != SseSendResult::Failed) result = SseSendResult::Sent;
    }

    xSemaphoreGive(sse_mutex_);
    return result;
}

void WebUI::poll_live_transport(size_t connected_clients,
                                size_t healthy_clients) {
    if (!live_) return;

    const uint32_t now_ms = millis();
    live_->poll(connected_clients, healthy_clients, now_ms);

    const char *payload = nullptr;
    size_t length = 0;
    uint32_t generation = observed_live_generation_;
    if (!live_->live_payload(payload, length, generation) || !length ||
        generation == observed_live_generation_) {
        return;
    }

    observed_live_generation_ = generation;
    if (events_ &&
        send_sse_to_clients(payload, "live", now_ms, false) ==
            SseSendResult::Failed) {
        sse_enforce_needed_ = true;
    }
}

void WebUI::handle_event(const RpcEvent &event) {
    if (event.kind == RpcEventKind::BootNotification) {
        if (events_ &&
            send_sse_to_clients("{}", "device_boot", millis(),
                                true) == SseSendResult::Failed) {
            sse_enforce_needed_ = true;
        }
    }

    if (!ManagementConsole::event_has_output(event)) return;

    StringPrint capture(AC_WEB_CONSOLE_COMMAND_OUTPUT_MAX);
    web_console_.handle_event(capture, event);
    if (!capture.text().length()) return;
    append_console_log(capture.text());
}

void WebUI::append_console_log(const String &text) {
    if (!text.length()) return;
    reserve_console_log();
    const char *data = text.c_str();
    const size_t original_len = text.length();
    size_t len = original_len;
    if (console_log_ && console_log_capacity_) {
        if (len >= console_log_capacity_) {
            data += len - console_log_capacity_;
            len = console_log_capacity_;
            console_log_start_ = 0;
            console_log_length_ = len;
            memcpy(console_log_, data, len);
        } else {
            const size_t free_space =
                console_log_capacity_ - console_log_length_;
            const size_t overflow = len > free_space ? len - free_space : 0;
            if (overflow) {
                console_log_start_ =
                    (console_log_start_ + overflow) % console_log_capacity_;
                console_log_length_ -= overflow;
            }

            size_t write_at =
                (console_log_start_ + console_log_length_) %
                console_log_capacity_;
            size_t first = console_log_capacity_ - write_at;
            if (first > len) first = len;
            memcpy(console_log_ + write_at, data, first);
            if (len > first) {
                memcpy(console_log_, data + first, len - first);
            }
            console_log_length_ += len;
        }
    }
    console_log_write_pos_ += original_len;
    console_seq_++;
}

void WebUI::reserve_console_log() {
    if (console_log_ || AC_WEB_CONSOLE_LOG_MAX == 0) return;
    console_log_ = static_cast<char *>(
        Memory::alloc_large(AC_WEB_CONSOLE_LOG_MAX));
    if (console_log_) {
        console_log_capacity_ = AC_WEB_CONSOLE_LOG_MAX;
    }
}

void WebUI::clear_console_log() {
    Memory::free(console_log_);
    console_log_ = nullptr;
    console_log_capacity_ = 0;
    console_log_start_ = 0;
    console_log_length_ = 0;
    console_log_write_pos_++;
    console_seq_++;
    console_sse_reset_pending_ = true;
}

uint64_t WebUI::console_log_begin_pos() const {
    return console_log_write_pos_ - console_log_length_;
}

void WebUI::append_console_log_json_range(LargeTextBuffer &json,
                                          uint64_t from,
                                          uint64_t to) const {
    if (!console_log_ || !console_log_capacity_ || from >= to) return;
    const uint64_t begin = console_log_begin_pos();
    if (from < begin) from = begin;
    if (to > console_log_write_pos_) to = console_log_write_pos_;
    if (from >= to) return;

    const size_t offset = static_cast<size_t>(from - begin);
    const size_t len = static_cast<size_t>(to - from);
    const size_t read_at =
        (console_log_start_ + offset) % console_log_capacity_;
    size_t first = console_log_capacity_ - read_at;
    if (first > len) first = len;
    append_json_escaped(json, console_log_ + read_at, first);
    if (len > first) {
        append_json_escaped(json, console_log_, len - first);
    }
}

void WebUI::build_console_json(LargeTextBuffer &json) const {
    const uint64_t begin = console_log_begin_pos();
    const uint64_t end = console_log_write_pos_;
    json = "{";
    json_add_uint64(json, "seq", console_seq_, false);
    json_add_uint64(json, "begin", begin);
    json_add_uint64(json, "end", end);
    json += ",\"log\":\"";
    append_console_log_json_range(json, begin, end);
    json += "\"}";
}

void WebUI::build_console_sse_json(LargeTextBuffer &json) const {
    const uint64_t begin = console_log_begin_pos();
    const uint64_t end = console_log_write_pos_;
    const bool reset = console_sse_reset_pending_ ||
                       console_sse_pos_ < begin ||
                       console_sse_pos_ > end;

    json = "{";
    json_add_uint64(json, "seq", console_seq_, false);
    if (reset) {
        json_add_bool(json, "reset", true);
        json_add_uint64(json, "begin", begin);
        json_add_uint64(json, "end", end);
        json += ",\"log\":\"";
        append_console_log_json_range(json, begin, end);
    } else {
        json_add_uint64(json, "from", console_sse_pos_);
        json_add_uint64(json, "to", end);
        json += ",\"append\":\"";
        append_console_log_json_range(json, console_sse_pos_, end);
    }
    json += "\"}";
}

void WebUI::note_console_sse_sent() {
    console_sse_seq_ = console_seq_;
    console_sse_pos_ = console_log_write_pos_;
    console_sse_reset_pending_ = false;
}

void WebUI::send_console_snapshot(AsyncWebServerRequest *request) const {
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache busy\"}");
        return;
    }
    LargeTextBuffer json;
    json.reserve(json_escaped_capacity(console_log_length_));
    build_console_json(json);
    if (json.overflowed()) {
        xSemaphoreGive(cache_mutex_);
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"console alloc\"}");
        return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        xSemaphoreGive(cache_mutex_);
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    xSemaphoreGive(cache_mutex_);
    request->send(response);
}

void WebUI::mark_snapshots_dirty(uint16_t mask) {
    snapshots_dirty_mask_ |= mask;
    if (mask & (SNAPSHOT_STATUS | SNAPSHOT_CONFIG | SNAPSHOT_AS11_BLE |
                SNAPSHOT_OXIMETRY | SNAPSHOT_SETTINGS | SNAPSHOT_OTA |
                SNAPSHOT_RESMED_OTA | SNAPSHOT_RESMED_REPOSITORY |
                SNAPSHOT_STORAGE_OPERATION | SNAPSHOT_REPORT)) {
        request_sse_push();
    }
}

void WebUI::request_sse_push() {
    sse_push_requested_.store(true, std::memory_order_release);
}

String WebUI::queued_json(const char *result) const {
    String json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_bool(json, "queued", true);
    json_add_string(json, "result", result ? result : "queued");
    json += '}';
    return json;
}

void WebUI::send_queue_result(AsyncWebServerRequest *request,
                              bool queued,
                              const char *result) const {
    if (queued) {
        request->send(202, "application/json", queued_json(result));
        return;
    }
    request->send(503, "application/json",
                  "{\"ok\":false,\"queued\":false,"
                  "\"error\":\"web command queue full\"}");
}

bool WebUI::request_allowed_cached(AsyncWebServerRequest *request) const {
    bool required = true;
    String user;
    String password;
    String whitelist;
    if (cache_mutex_ &&
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        required = cached_http_auth_required_;
        user = cached_http_user_;
        password = cached_http_password_;
        whitelist = cached_auth_whitelist_;
        xSemaphoreGive(cache_mutex_);
    }
    if (!required) return true;
    if (request && request->client() &&
        auth_whitelist_matches(request->client()->remoteIP(), whitelist)) {
        return true;
    }
    return request && request->authenticate(user.c_str(), password.c_str());
}

bool WebUI::enqueue_command(WebCommand &&command) {
    const uint8_t kind = command.kind;
    if (!command_mutex_) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue unavailable kind=%s\n",
                  web_command_name(kind));
        return false;
    }

    if (xSemaphoreTake(command_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue busy kind=%s\n",
                  web_command_name(kind));
        return false;
    }
    const bool queued = command_queue_.push(std::move(command));
    xSemaphoreGive(command_mutex_);
    if (!queued) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue full kind=%s\n",
                  web_command_name(kind));
    }
    return queued;
}

bool WebUI::enqueue_simple_command(uint8_t kind) {
    WebCommand command;
    command.kind = kind;
    return enqueue_command(std::move(command));
}

void WebUI::drain_commands() {
    if (!command_mutex_) return;
    for (size_t i = 0; i < AC_WEB_COMMANDS_PER_POLL; ++i) {
        WebCommand command;
        if (xSemaphoreTake(command_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
            return;
        }
        const bool has_command = command_queue_.pop(command);
        xSemaphoreGive(command_mutex_);
        if (!has_command) break;
        execute_command(command);
    }
}

void WebUI::publish_snapshots(bool force, PollCheckpoint checkpoint) {
    const uint32_t now = millis();
    const bool periodic_due =
        static_cast<int32_t>(now - last_snapshot_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);

    uint16_t rebuild_mask = snapshots_dirty_mask_;
    if (force || !snapshots_ready_) {
        rebuild_mask = SNAPSHOT_ALL;
    } else if (periodic_due) {
        rebuild_mask |= SNAPSHOT_PERIODIC;
    }
    if (!rebuild_mask) return;

    next_status_json_.clear();
    next_exports_json_.clear();
    for (SnapshotChannel &channel : snapshot_channels_) {
        channel.next.clear();
    }
    uint16_t completed_mask = 0;

    if (rebuild_mask & SNAPSHOT_STATUS) {
        uint32_t revision = observed_status_revision_;
        if (status_ && status_->copy_snapshot(next_status_json_, revision)) {
            observed_status_revision_ = revision;
            completed_mask |= SNAPSHOT_STATUS;
        }
        if (checkpoint) checkpoint("web_ui.snapshots.status_copy");
    }

    if (rebuild_mask & SNAPSHOT_EXPORTS) {
        if (exports_ && exports_->build_status_snapshot(next_exports_json_)) {
            completed_mask |= SNAPSHOT_EXPORTS;
        }
        if (checkpoint) checkpoint("web_ui.snapshots.exports_copy");
    }

    for (SnapshotChannel &channel : snapshot_channels_) {
        if (!(rebuild_mask & channel.mask)) continue;

        uint32_t revision = channel.observed_revision;
        if (channel.source &&
            channel.source->copy(channel.next, revision)) {
            channel.observed_revision = revision;
            completed_mask |= channel.mask;
        }
        if (checkpoint) checkpoint(channel.checkpoint);
    }
    if (!completed_mask || !cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        return;
    }

    if (completed_mask & SNAPSHOT_STATUS) {
        cached_status_json_.swap(next_status_json_);
    }
    if (completed_mask & SNAPSHOT_EXPORTS) {
        cached_exports_json_.swap(next_exports_json_);
    }

    for (SnapshotChannel &channel : snapshot_channels_) {
        if (completed_mask & channel.mask) {
            channel.cached.swap(channel.next);
        }
    }
    snapshots_dirty_mask_ &= ~completed_mask;
    snapshots_ready_ = snapshots_dirty_mask_ == 0;
    if (force || periodic_due || (completed_mask & SNAPSHOT_PERIODIC)) {
        last_snapshot_ms_ = now;
    }
    xSemaphoreGive(cache_mutex_);
}

void WebUI::execute_command(WebCommand &command) {
    switch (command.kind) {
        case WebCommandConsoleLine:
            execute_console_line(command.text);
            break;
        case WebCommandConsoleClear:
            clear_console_log();
            break;
        default:
            break;
    }
}

void WebUI::execute_console_line(const std::string &line) {
    String command(line.c_str());
    command.trim();
    if (!command.length()) return;
    if (!console_router_) return;

    StringPrint capture(AC_WEB_CONSOLE_COMMAND_OUTPUT_MAX);
    web_console_.execute_line(command, capture, *console_router_);
    String entry = "> ";
    entry += command;
    entry += "\n";
    entry += capture.text();
    append_console_log(entry);
}

void WebUI::register_routes(HttpRouteModule *const *route_modules,
                            size_t route_module_count) {
    // Static UI and snapshots
    server_->on("/", HTTP_GET, send_web_ui);
    server_->on(AsyncURIMatcher::exact("/wizard"), HTTP_GET, send_web_ui);

    // Web console and file log
    server_->on(
        AsyncURIMatcher::exact("/api/console"), HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_console_snapshot(request);
        });

    server_->on(
        AsyncURIMatcher::exact("/api/console"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;

            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }

            String command;

            if (!json_variant_to_string(doc["cmd"], command)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing cmd\"}");
                return;
            }

            command.trim();

            if (!command.length()) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"empty cmd\"}");
                return;
            }

            WebCommand queued;
            queued.kind = WebCommandConsoleLine;
            queued.text = command.c_str();

            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, http_request_body_handler);

    server_->on(
        AsyncURIMatcher::exact("/api/console/clear"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_queue_result(request,
                              enqueue_simple_command(WebCommandConsoleClear));
        });

    for (size_t i = 0; i < route_module_count; ++i) {
        if (route_modules && route_modules[i]) {
            route_modules[i]->register_routes(*server_);
        }
    }

    server_->onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "application/json",
                      "{\"ok\":false,\"error\":\"not found\"}");
    });
}

}  // namespace aircannect
