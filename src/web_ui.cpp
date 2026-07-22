#include "web_ui.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <algorithm>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <time.h>
#include <utility>

#include "auth_utils.h"
#include "app_config_registry.h"
#include "as11_rpc.h"
#include "board.h"
#include "debug_log.h"
#include "http_route_module.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "memory_manager.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_manager.h"
#include "system_status_snapshot.h"
#include "string_util.h"
#include "string_print.h"
#include "version.h"
#include "web_ui_html.h"

namespace aircannect {

namespace {

static constexpr size_t WEB_JSON_RESERVE_SMALL = 512;
static constexpr size_t WEB_JSON_RESERVE_MEDIUM = 1024;
static constexpr size_t WEB_CONFIG_FULL_JSON_RESERVE = 2304;
static constexpr size_t WEB_CONFIG_SCHEMA_JSON_RESERVE = 12 * 1024;
static constexpr size_t WEB_CONFIG_SYNC_JSON_RESERVE = 640;
static constexpr size_t WEB_CONFIG_SMALL_JSON_RESERVE = 384;
static constexpr uint32_t WEB_LIVE_VIEW_LEASE_MS = 12000;

const char *web_command_name(uint8_t kind) {
    switch (kind) {
        case WebCommandConsoleLine: return "console_line";
        case WebCommandConsoleClear: return "console_clear";
        case WebCommandConfigUpdate: return "config_update";
        case WebCommandWifiUpdate: return "wifi_update";
        case WebCommandTimeAction: return "time_action";
        case WebCommandTherapyAction: return "therapy_action";
        case WebCommandOximetryAction: return "oximetry_action";
        default: return "unknown";
    }
}

bool web_config_section_known(const char *section) {
    if (!section || !section[0] || strcmp(section, "all") == 0) return true;
    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(section, app_config_group_id(fields[i].group)) == 0) {
            return true;
        }
    }
    return false;
}

bool web_config_section_includes(const char *section, AppConfigGroup group) {
    return !section || !section[0] || strcmp(section, "all") == 0 ||
           strcmp(section, app_config_group_id(group)) == 0;
}

size_t json_escaped_capacity(size_t raw_len, size_t overhead = 128) {
    // JSON string escaping can expand a control byte to "\\u00XX".
    return overhead + raw_len * 6;
}

uint32_t fnv1a32_string(const String &text) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < text.length(); ++i) {
        hash ^= static_cast<uint8_t>(text[i]);
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

size_t web_config_json_reserve(const char *section) {
    if (!section || !section[0] || strcmp(section, "all") == 0) {
        return 4096;
    }
    if (strcmp(section, "access") == 0 ||
        strcmp(section, "logging") == 0) {
        return 2048;
    }
    if (strcmp(section, "smb") == 0 ||
        strcmp(section, "sleephq") == 0) {
        return WEB_CONFIG_SYNC_JSON_RESERVE;
    }
    return WEB_CONFIG_SMALL_JSON_RESERVE;
}

const char *web_config_field_type_name(AppConfigFieldType type) {
    switch (type) {
        case AppConfigFieldType::Bool: return "bool";
        case AppConfigFieldType::UInt16: return "number";
        case AppConfigFieldType::String: return "text";
        case AppConfigFieldType::Secret: return "password";
        case AppConfigFieldType::Enum:
        case AppConfigFieldType::LogLevel:
            return "enum";
    }
    return "text";
}

bool web_config_value_text(JsonVariantConst value,
                           const AppConfigFieldDescriptor &field,
                           String &out) {
    out = "";
    switch (field.type) {
        case AppConfigFieldType::Bool:
            if (value.is<bool>()) {
                out = value.as<bool>() ? "1" : "0";
                return true;
            }
            if (value.is<const char *>()) {
                out = value.as<const char *>();
                return true;
            }
            return false;

        case AppConfigFieldType::UInt16:
            if (value.is<int>()) {
                const int parsed = value.as<int>();
                if (parsed < 0 || parsed > 65535) return false;
                out = String(parsed);
                return true;
            }
            if (value.is<const char *>()) {
                out = value.as<const char *>();
                return true;
            }
            return false;

        case AppConfigFieldType::String:
        case AppConfigFieldType::Secret:
        case AppConfigFieldType::Enum:
        case AppConfigFieldType::LogLevel:
            if (!value.is<const char *>()) return false;
            out = value.as<const char *>();
            return true;
    }
    return false;
}

static constexpr AppConfigEnumValue WEB_LOG_LEVEL_VALUES[] = {
    {"ERROR", "Error"},
    {"WARN", "Warn"},
    {"INFO", "Info"},
    {"DEBUG", "Debug"},
};

void append_config_schema_enum(LargeTextBuffer &json,
                               const AppConfigFieldDescriptor &field) {
    const AppConfigEnumValue *values = field.enum_values;
    size_t count = field.enum_value_count;
    if (field.type == AppConfigFieldType::LogLevel) {
        values = WEB_LOG_LEVEL_VALUES;
        count = sizeof(WEB_LOG_LEVEL_VALUES) / sizeof(WEB_LOG_LEVEL_VALUES[0]);
    }
    if (!values || !count) return;

    json += ",\"enum\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i) json += ',';
        json += "{";
        json_add_string(json, "value", values[i].value, false);
        json_add_string(json, "label", values[i].label);
        json += "}";
    }
    json += "]";
}

bool append_le32(ReportSpoolBuffer &out, uint32_t value) {
    size_t offset = 0;
    uint8_t *dst = out.append_uninitialized(4, offset);
    if (!dst) return false;
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    return true;
}

bool json_get_string(JsonDocument &doc, const char *key, String &out) {
    if (!doc[key].is<const char *>()) return false;
    out = doc[key].as<const char *>();
    return true;
}

bool motor_hours(std::string_view iso_duration,
                 char *out,
                 size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    if (iso_duration.size() < 4 || iso_duration.substr(0, 2) != "PT") {
        return false;
    }
    size_t start = 2;
    const size_t end = iso_duration.find('S', start);
    if (end == std::string_view::npos || end == start) return false;
    uint64_t seconds = 0;
    for (size_t i = start; i < end; ++i) {
        const char c = iso_duration[i];
        if (c < '0' || c > '9') return false;
        seconds = seconds * 10 + static_cast<unsigned>(c - '0');
    }
    const unsigned long hours =
        static_cast<unsigned long>((seconds + 1800) / 3600);
    snprintf(out, out_size, "%lu", hours);
    return true;
}

const char *stream_command_name(StreamCommandType type) {
    switch (type) {
        case StreamCommandType::Start: return "start";
        case StreamCommandType::Stop: return "stop";
        case StreamCommandType::None:
        default: return "none";
    }
}

const char *oximetry_source_name(OximetrySource source) {
    switch (source) {
        case OximetrySource::None: return "none";
        case OximetrySource::Udp: return "udp";
        case OximetrySource::Ble: return "ble";
        default: return "unknown";
    }
}

const char *oximetry_sensor_state_name(OximetrySensorState state) {
    switch (state) {
        case OximetrySensorState::Off: return "off";
        case OximetrySensorState::Idle: return "idle";
        case OximetrySensorState::Scanning: return "scanning";
        case OximetrySensorState::Connecting: return "connecting";
        case OximetrySensorState::Connected: return "connected";
        case OximetrySensorState::Streaming: return "streaming";
        default: return "unknown";
    }
}

template <typename JsonOut>
void append_oximetry_sensor(JsonOut &json,
                            const OximetrySensorDevice &device,
                            size_t index,
                            bool include_index) {
    json += '{';
    if (include_index) json_add_int(json, "index", index, false);
    else json_add_string(json, "addr", device.addr, false);
    if (include_index) json_add_string(json, "addr", device.addr);
    json_add_int(json, "addr_type", device.addr_type);
    json_add_string(json, "name", device.name);
    json_add_int(json, "rssi", device.rssi);
    json_add_bool(json, "autoconnect", device.autoconnect);
    json += '}';
}

template <typename JsonOut>
void append_json_float_value(JsonOut &json, float value) {
    append_json_float(json, value);
}

template <typename JsonOut>
void append_live_series(JsonOut &json,
                        const char *key,
                        const LiveChartSeriesBatch &series,
                        bool comma = true) {
    if (comma) json += ',';
    json += '"';
    json += key;
    json += "\":[";
    const size_t count =
        series.count <= series.capacity ? series.count : series.capacity;
    for (size_t i = 0; series.values && series.valid && i < count; ++i) {
        if (i) json += ',';
        if (!series.valid[i]) {
            json += "null";
        } else {
            append_json_float_value(json, series.values[i]);
        }
    }
    json += ']';
}

}  // namespace

bool WebUI::begin(RpcRequestPort &rpc,
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
                  uint16_t port) {
    if (started_) return true;
    stop();
    rpc_ = &rpc;
    stream_ = &stream;
    device_ = &device;
    wifi_manager_ = &wifi_manager;
    tcp_bridge_ = &tcp_bridge;
    config_service_ = &config_service;
    observed_config_revision_ = config_service.revision();
    time_sync_service_ = &time_sync_service;
    ota_manager_ = &ota_manager;
    session_manager_ = &session_manager;
    sink_manager_ = &sink_manager;
    oximetry_manager_ = &oximetry_manager;
    console_ctx_ = &console_ctx;

    if (!command_mutex_) {
        command_mutex_ = xSemaphoreCreateMutexStatic(&command_mutex_storage_);
    }
    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!sse_mutex_) {
        sse_mutex_ = xSemaphoreCreateMutexStatic(&sse_mutex_storage_);
    }
    if (!live_view_mutex_) {
        live_view_mutex_ =
            xSemaphoreCreateMutexStatic(&live_view_mutex_storage_);
    }
    if (!command_mutex_ || !cache_mutex_ || !sse_mutex_ ||
        !live_view_mutex_) {
        stop();
        return false;
    }
    reserve_cached_json();

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

void WebUI::reserve_cached_json() {
    cached_status_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    cached_stream_json_.reserve(AC_WEB_STREAM_JSON_RESERVE);
    cached_wifi_json_.reserve(AC_WEB_WIFI_JSON_RESERVE);
    cached_oximetry_sensors_json_.reserve(
        AC_WEB_OXIMETRY_SENSORS_JSON_RESERVE);
    live_json_.reserve(4096);
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
                ref.client = nullptr;
                ref.connected_ms = 0;
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
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return out;
    }
    out.status = capture(cached_status_json_);
    out.stream = capture(cached_stream_json_);
    out.console.length = console_log_length_;
    out.console.capacity = console_log_capacity_;
    out.wifi = capture(cached_wifi_json_);
    out.oximetry_sensors = capture(cached_oximetry_sensors_json_);
    out.live = capture(live_json_);
    out.console_log_length = console_log_length_;
    xSemaphoreGive(cache_mutex_);
    return out;
}

void WebUI::stop() {
    if (sink_manager_) sink_manager_->set_live_chart_enabled(false);
    web_console_.cancel_pending_storage();
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

    if (live_view_mutex_) {
        if (xSemaphoreTake(live_view_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (LiveViewLease &lease : live_view_leases_) lease = {};
            xSemaphoreGive(live_view_mutex_);
        }
    } else {
        for (LiveViewLease &lease : live_view_leases_) lease = {};
    }

    sse_enforce_needed_ = false;
    live_last_send_ms_ = 0;
    live_json_ = "";
    Memory::free(console_log_);
    console_log_ = nullptr;
    console_log_capacity_ = 0;
    console_log_start_ = 0;
    console_log_length_ = 0;
    console_log_write_pos_ = 0;
    console_sse_pos_ = 0;
    console_sse_reset_pending_ = false;
    if (sink_manager_) {
        sink_manager_->set_live_chart_enabled(false);
        sink_manager_->clear_live_chart_batch();
    }
    snapshots_ready_ = false;
    snapshots_dirty_mask_ = SNAPSHOT_ALL;
    observed_device_revision_ = 0;
    observed_config_revision_ = 0;
    last_snapshot_ms_ = 0;
    last_sse_push_ms_ = 0;
    sse_push_requested_ = false;
    stream_ = nullptr;
    started_ = false;
}

void WebUI::poll(PollCheckpoint checkpoint) {
    if (!started_) return;
    const bool realtime_active =
        stream_ &&
        (stream_->activity_active(millis(), AC_WIFI_ROAM_STREAM_QUIET_MS) ||
         (device_ && device_->state().therapy_state() ==
                         As11TherapyState::Running));
    if (device_ && observed_device_revision_ != device_->revision()) {
        observed_device_revision_ = device_->revision();
        mark_snapshots_dirty(SNAPSHOT_STATUS);
    }
    if (config_service_ &&
        observed_config_revision_ != config_service_->revision()) {
        observed_config_revision_ = config_service_->revision();
        mark_snapshots_dirty(SNAPSHOT_ALL);
    }

    if (web_console_.storage_output_pending()) {
        StringPrint capture(AC_FILE_LOG_TAIL_READ_CHUNK);
        web_console_.poll_pending(capture);
        if (capture.text().length()) append_console_log(capture.text());
    }

    drain_commands();
    if (checkpoint) checkpoint("web_ui.commands");
    enforce_sse_limits();
    if (checkpoint) checkpoint("web_ui.sse_limits");
    poll_live_stream();
    if (checkpoint) checkpoint("web_ui.live");
    publish_snapshots(false, realtime_active, checkpoint);
    if (checkpoint) checkpoint("web_ui.snapshots");

    if (!events_ || events_->count() == 0) return;
    const bool push_requested = sse_push_requested_;
    if (!push_requested &&
        static_cast<int32_t>(millis() - last_sse_push_ms_) <
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS)) {
        if (checkpoint) checkpoint("web_ui.sse_idle");
        return;
    }
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        sse_push_requested_ = push_requested;
        if (checkpoint) checkpoint("web_ui.sse_lock");
        return;
    }
    last_sse_push_ms_ = millis();
    sse_push_requested_ = false;
    const uint32_t event_id = millis();
    bool sse_backpressure = false;
    if (send_sse_to_clients(cached_status_json_.c_str(), "status", event_id,
                            true) == SseSendResult::Failed) {
        sse_backpressure = true;
    }
    if (send_sse_to_clients(cached_stream_json_.c_str(), "stream", event_id,
                            false) == SseSendResult::Failed) {
        sse_backpressure = true;
    }
    if (console_sse_seq_ != console_seq_) {
        // Console output can be chatty during RPC activity, so it shares the
        // same throttled SSE cadence as status/stream updates.
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

void WebUI::handle_sse_connect(AsyncEventSourceClient *client) {
    if (!client || !sse_mutex_) return;

    bool stored = false;
    if (xSemaphoreTake(sse_mutex_, portMAX_DELAY) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client && !ref.client->connected()) {
                ref.client = nullptr;
                ref.connected_ms = 0;
                ref.last_status_ms = 0;
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
                ref.client = client;
                ref.connected_ms = millis();
                ref.last_status_ms = 0;
                stored = true;
                break;
            }
        }
        sse_enforce_needed_ = true;
        xSemaphoreGive(sse_mutex_);
    }

    if (!stored) {
        client->close();
    }
}

void WebUI::handle_sse_disconnect(AsyncEventSourceClient *client) {
    if (!client || !sse_mutex_) return;

    if (xSemaphoreTake(sse_mutex_, portMAX_DELAY) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client != client) continue;
            ref.client = nullptr;
            ref.connected_ms = 0;
            ref.last_status_ms = 0;
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
                ref.client = nullptr;
                ref.connected_ms = 0;
                ref.last_status_ms = 0;
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

size_t WebUI::healthy_sse_client_count() {
    if (!sse_mutex_) return 0;
    size_t count = 0;
    if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return 0;
    }
    for (SseClientRef &ref : sse_clients_) {
        AsyncEventSourceClient *client = ref.client;
        if (!client) continue;
        if (!client->connected()) {
            ref.client = nullptr;
            ref.connected_ms = 0;
            ref.last_status_ms = 0;
            continue;
        }
        if (client->packetsWaiting() <= AC_WEB_SSE_CLIENT_PENDING_MAX) {
            count++;
        }
    }
    xSemaphoreGive(sse_mutex_);
    return count;
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
            ref.client = nullptr;
            ref.connected_ms = 0;
            ref.last_status_ms = 0;
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

void WebUI::poll_live_stream() {
    const size_t clients = healthy_sse_client_count();
    const bool live_needed = live_view_requested(millis()) && clients > 0;
    if (sink_manager_) sink_manager_->set_live_chart_enabled(live_needed);
    if (!live_needed) {
        if (sink_manager_) sink_manager_->clear_live_chart_batch();
        live_last_send_ms_ = 0;
        return;
    }
    send_live_batch(millis());
}

bool WebUI::live_view_requested(uint32_t now_ms) {
    if (!live_view_mutex_) return false;
    bool active = false;
    if (xSemaphoreTake(live_view_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return false;
    }
    for (LiveViewLease &lease : live_view_leases_) {
        if (!lease.client_hash) continue;
        if (static_cast<int32_t>(now_ms - lease.expires_ms) >= 0) {
            lease = {};
            continue;
        }
        active = true;
    }
    xSemaphoreGive(live_view_mutex_);
    return active;
}

void WebUI::send_live_batch(uint32_t now_ms) {
    if (!events_ || !sink_manager_) return;
    const LiveChartRuntimeStatus &live = sink_manager_->live_chart_status();
    const bool has_samples =
        live.pressure.count || live.flow.count || live.leak.count ||
        live.inspiratory_pressure.count ||
        live.expiratory_pressure.count ||
        live.spo2.count || live.pulse.count;
    const bool interval_due =
        static_cast<int32_t>(now_ms - live_last_send_ms_) >=
        static_cast<int32_t>(AC_WEB_LIVE_PUSH_INTERVAL_MS);
    const bool heartbeat_due =
        static_cast<int32_t>(now_ms - live_last_send_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    if (has_samples && !interval_due) return;
    if (!has_samples && !live.state_dirty && !heartbeat_due) return;
    if (healthy_sse_client_count() == 0) {
        sink_manager_->clear_live_chart_batch();
        return;
    }

    live_json_ = "{";
    json_add_int(live_json_, "seq", static_cast<long>(++live_seq_), false);
    json_add_bool(live_json_, "active", live.desired);
    json_add_bool(live_json_, "attached", live.attached);
    json_add_int(live_json_, "frames", static_cast<long>(live.frames));
    json_add_int(live_json_, "drops", static_cast<long>(live.drops));
    json_add_int(live_json_, "attach_failures",
                 static_cast<long>(live.attach_failures));
    if (live.last_frame_ms) {
        json_add_int(live_json_, "last_age_ms", now_ms - live.last_frame_ms);
    } else {
        live_json_ += ",\"last_age_ms\":null";
    }
    json_add_string(live_json_, "last_error", live.last_error);
    live_json_ += ",\"samples\":{";
    append_live_series(live_json_, "pressure", live.pressure, false);
    append_live_series(live_json_, "flow", live.flow);
    append_live_series(live_json_, "leak", live.leak);
    append_live_series(live_json_, "inspiratory_pressure",
                       live.inspiratory_pressure);
    append_live_series(live_json_, "expiratory_pressure",
                       live.expiratory_pressure);
    append_live_series(live_json_, "spo2", live.spo2);
    append_live_series(live_json_, "pulse", live.pulse);
    live_json_ += "}}";

    if (send_sse_to_clients(live_json_.c_str(), "live", now_ms,
                            false) == SseSendResult::Failed) {
        sse_enforce_needed_ = true;
    }
    live_last_send_ms_ = now_ms;
    sink_manager_->mark_live_chart_sent();
}

void WebUI::handle_event(const RpcEvent &event) {
    if (event.kind == RpcEventKind::BootNotification) {
        mark_snapshots_dirty(SNAPSHOT_STATUS);
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
    if (mask & SNAPSHOT_STATUS) request_sse_push();
}

void WebUI::request_sse_push() {
    sse_push_requested_ = true;
}

String WebUI::queued_json(const char *result) const {
    String json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_bool(json, "queued", true);
    json_add_string(json, "result", result ? result : "queued");
    json += '}';
    return json;
}

void WebUI::send_cached(AsyncWebServerRequest *request,
                        const LargeTextBuffer &json) const {
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache busy\"}");
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

void WebUI::send_config_json(AsyncWebServerRequest *request,
                             const char *section) const {
    if (!web_config_section_known(section)) {
        request->send(404, "application/json",
                      "{\"ok\":false,\"error\":\"unknown_section\"}");
        return;
    }

    LargeTextBuffer json;
    if (!json.reserve(web_config_json_reserve(section))) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"config_alloc\"}");
        return;
    }
    build_config_json(json, section);
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"config_overflow\"}");
        return;
    }

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
}

void WebUI::send_config_schema_json(AsyncWebServerRequest *request) const {
    LargeTextBuffer json;
    if (!json.reserve(WEB_CONFIG_SCHEMA_JSON_RESERVE)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"schema_alloc\"}");
        return;
    }
    build_config_schema_json(json);
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"schema_overflow\"}");
        return;
    }

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
}

void WebUI::send_config_update(AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!http_parse_json_body(request, doc, body)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }
    WebCommand queued;
    queued.kind = WebCommandConfigUpdate;
    queued.body = std::move(body);
    send_queue_result(request, enqueue_command(std::move(queued)));
}

void WebUI::send_live_view_state(AsyncWebServerRequest *request) {
    bool active = false;
    if (request->hasArg("active")) {
        parse_bool_yesno(request->arg("active"), active);
    } else if (request->hasArg("enabled")) {
        parse_bool_yesno(request->arg("enabled"), active);
    }
    const uint32_t client_hash =
        request->hasArg("id") ? fnv1a32_string(request->arg("id")) : 1u;
    const uint32_t now_ms = millis();
    if (live_view_mutex_ &&
        xSemaphoreTake(live_view_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        LiveViewLease *empty = nullptr;
        LiveViewLease *oldest = nullptr;
        for (LiveViewLease &lease : live_view_leases_) {
            if (lease.client_hash == client_hash) {
                if (active) {
                    lease.expires_ms = now_ms + WEB_LIVE_VIEW_LEASE_MS;
                } else {
                    lease = {};
                }
                xSemaphoreGive(live_view_mutex_);
                request->send(200, "application/json",
                              active ? "{\"ok\":true,\"active\":true}"
                                     : "{\"ok\":true,\"active\":false}");
                return;
            }
            if (!lease.client_hash) {
                if (!empty) empty = &lease;
                continue;
            }
            if (static_cast<int32_t>(now_ms - lease.expires_ms) >= 0) {
                lease = {};
                if (!empty) empty = &lease;
                continue;
            }
            if (!oldest ||
                static_cast<int32_t>(lease.expires_ms -
                                     oldest->expires_ms) < 0) {
                oldest = &lease;
            }
        }
        if (active) {
            LiveViewLease *slot = empty ? empty : oldest;
            if (slot) {
                slot->client_hash = client_hash;
                slot->expires_ms = now_ms + WEB_LIVE_VIEW_LEASE_MS;
            }
        }
        xSemaphoreGive(live_view_mutex_);
    }
    request->send(200, "application/json",
                  active ? "{\"ok\":true,\"active\":true}"
                         : "{\"ok\":true,\"active\":false}");
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

void WebUI::build_status_json(LargeTextBuffer &json,
                              PollCheckpoint checkpoint) const {
    const SystemStatusSnapshot snap = collect_system_status({
        *device_,
        *wifi_manager_,
        config_service_->data(),
        *time_sync_service_,
        *ota_manager_,
        *oximetry_manager_,
    }, checkpoint);
    const MemoryStatus &mem = snap.memory;
    const StorageStatus &storage = snap.storage;
    const WifiStatusSnapshot &wifi = snap.wifi;
    const As11StatusSnapshot &as11 = snap.as11;
    const OximetryRuntimeStatus &oxi = snap.oximetry;
    const TimeStatusSnapshot &time = snap.time;

    json = "{";
    json_add_string(json, "version", snap.version, false);
    json_add_string(json, "built", snap.built);
    json_add_string(json, "hostname",
                    config_service_->data().hostname.c_str());
    json_add_int(json, "uptime", snap.uptime_s);
    json_add_int(json, "heap", static_cast<long>(mem.heap_free));
    json_add_bool(json, "psram_available", mem.psram_available);
    json_add_int(json, "psram_free", static_cast<long>(mem.psram_free));
    json_add_string(json, "storage_state",
                    Storage::state_name(storage.state));
    json_add_uint64(json, "storage_total", storage.total_bytes);
    json_add_uint64(json, "storage_used", storage.used_bytes);
    json_add_string_view(json, "wifi_state", wifi.state);
    json_add_string_view(json, "wifi_ssid", wifi.ssid);
    json_add_string(json, "wifi_ip", wifi.ip);
    json_add_int(json, "wifi_rssi", wifi.rssi);
    json_add_int(json, "wifi_channel", wifi.channel);
    json_add_string(json, "wifi_bssid", wifi.bssid);
    json_add_int(json, "wifi_profile", wifi.active_profile);
    json_add_bool(json, "wifi_roam", wifi.roaming_enabled);
    json_add_bool(json, "update_checking", snap.update.checking);
    json_add_bool(json, "update_available", snap.update.available);
    json_add_string(json, "update_version", snap.update.version);
    json_add_string_view(json, "device_name", as11.product_name);
    json_add_string_view(json, "serial", as11.serial_number);
    json_add_string_view(json, "software_id", as11.software_identifier);
    json_add_string(json, "therapy",
                    As11DeviceState::therapy_state_name(as11.therapy_state));
    json_add_string(json, "therapy_pending",
                    As11DeviceState::therapy_target_name(
                        as11.pending_therapy_target));
    json_add_string_view(json, "profile", as11.active_therapy_profile);
    char hours[16];
    motor_hours(as11.motor_run_meter, hours, sizeof(hours));
    json_add_string(json, "motor_hours", hours);
    json += ",\"oximetry\":{";
    json_add_bool(json, "enabled", oxi.enabled, false);
    json_add_string(json, "source", oximetry_source_name(oxi.source));
    json_add_string(json, "source_detail", oxi.source_detail);
    json_add_bool(json, "source_present", oxi.source_present);
    json_add_bool(json, "source_fresh", oxi.source_fresh);
    json_add_bool(json, "valid", oxi.reading.valid);
    json_add_bool(json, "contact_known", oxi.reading.contact_known);
    json_add_bool(json, "contact_present", oxi.reading.contact_present);
    if (oxi.reading.valid) {
        json_add_int(json, "spo2", oxi.reading.spo2);
        json_add_int(json, "pulse_bpm", oxi.reading.pulse_bpm);
    } else {
        json += ",\"spo2\":null,\"pulse_bpm\":null";
    }
    json_add_int(json, "source_age_ms", oxi.last_source_age_ms);
    json_add_string(json, "advertise_mode",
                    oximetry_advertise_mode_name(oxi.advertise_mode));
    json_add_bool(json, "manual_advertising_requested",
                  oxi.manual_advertising_requested);
    json_add_bool(json, "ble_available", oxi.ble_available);
    json_add_bool(json, "advertising", oxi.advertising);
    json_add_bool(json, "connected", oxi.connected);
    json_add_bool(json, "subscribed", oxi.subscribed);
    json_add_bool(json, "pairing_active", oxi.pairing_active);
    json_add_int(json, "pairing_left_ms", oxi.pairing_left_ms);
    json_add_string(json, "ble_name", oxi.ble_name);
    json_add_string(json, "ble_peer", oxi.ble_peer);
    json += '}';
    json_add_string_view(json, "device_datetime", as11.device_datetime);
    if (as11.clock_valid) {
        json_add_int(json, "device_datetime_age_ms",
                     snap.now_ms - as11.clock_sample_ms);
    } else {
        json += ",\"device_datetime_age_ms\":null";
    }
    json_add_bool(json, "resmed_time_sync_enabled",
                  time.resmed_time_sync_enabled);
    json_add_bool(json, "ntp_synced", time.ntp_synced);
    json_add_bool(json, "esp_time_valid", time.esp_time_valid);
    json_add_string_view(json, "esp_time_source", time.esp_time_source);
    json_add_string(json, "esp_datetime", time.esp_datetime);
    json += '}';
}

void WebUI::build_oximetry_sensors_json(LargeTextBuffer &json) const {
    const OximetryRuntimeStatus runtime = oximetry_manager_->runtime_status();
    const OximetrySensorStatus sensor = oximetry_manager_->sensor_status();
    OximetrySensorDevice oxi_scan[AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
    OximetrySensorDevice oxi_known[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    const size_t oxi_scan_count =
        oximetry_manager_->sensor_scan_results(
            oxi_scan, AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS);
    const size_t oxi_known_count =
        oximetry_manager_->known_sensors(oxi_known,
                                         AC_OXIMETRY_SENSOR_MAX_KNOWN);

    json = "{";
    json_add_bool(json, "enabled", runtime.enabled, false);
    json_add_bool(json, "ble_available", runtime.ble_available);
    json_add_string(json, "sensor_state",
                    oximetry_sensor_state_name(sensor.sensor_state));
    json_add_bool(json, "sensor_task_started", sensor.sensor_task_started);
#if AC_STACK_PROFILE_ENABLED
    json_add_int(json, "sensor_task_stack_free",
                 static_cast<long>(
                     sensor.sensor_task_stack_high_water_bytes));
#endif
    json_add_bool(json, "sensor_scanning", sensor.sensor_scanning);
    json_add_bool(json, "sensor_connected", sensor.sensor_connected);
    json_add_int(json, "sensor_known_count", sensor.sensor_known_count);
    json_add_int(json, "sensor_scan_count", sensor.sensor_scan_count);
    json_add_int(json, "sensor_scan_generation",
                 sensor.sensor_scan_generation);
    json_add_string(json, "sensor_peer", sensor.sensor_peer);
    json_add_string(json, "sensor_name", sensor.sensor_name);
    json += ",\"sensor_scan_results\":[";
    for (size_t i = 0; i < oxi_scan_count; ++i) {
        if (i) json += ',';
        append_oximetry_sensor(json, oxi_scan[i], i, true);
    }
    json += "],\"sensor_known\":[";
    for (size_t i = 0; i < oxi_known_count; ++i) {
        if (i) json += ',';
        append_oximetry_sensor(json, oxi_known[i], i, false);
    }
    json += "]}";
}

void WebUI::build_stream_json(LargeTextBuffer &json) const {
    const StreamBroker &stream = *stream_;
    const LiveChartRuntimeStatus &live = sink_manager_->live_chart_status();

    json = "{";
    json_add_bool(json, "desired", stream.desired_active(), false);
    json_add_bool(json, "subscribed", stream.actual_active());
    json_add_bool(json, "pending_start", stream.pending_start());
    json_add_bool(json, "pending_stop", stream.pending_stop());
    json_add_bool(json, "error", stream.error());
    json_add_string(json, "error_command",
                    stream_command_name(stream.error_command()));
    json_add_int(json, "consumers", stream.consumer_count());
    json_add_int(json, "published_payloads", stream.published_payloads());
    json_add_int(json, "fanout_targets", stream.fanout_targets());
    json_add_int(json, "fanout_drops", stream.total_queue_drops());
    json_add_int(json, "frame_pool_used", stream.frame_pool_in_use());
    json_add_int(json, "frame_pool_capacity", stream.frame_pool_capacity());
    json_add_int(json, "parse_errors", stream.parse_errors());
    json_add_int(json, "pool_exhaustions", stream.pool_exhaustions());
    json_add_int(json, "truncated_frames", stream.truncated_frames());
    json_add_bool(json, "web_live_attached", live.attached);
    json_add_int(json, "web_live_handle", live.handle);
    json_add_int(json, "web_live_frames", static_cast<long>(live.frames));
    json_add_int(json, "web_live_drops", static_cast<long>(live.drops));
    json_add_int(json, "web_live_attach_failures",
                 static_cast<long>(live.attach_failures));
    json_add_string(json, "web_live_error", live.last_error);
    json_add_int(json, "stream_id", stream.last_stream_id());
    json_add_int(json, "start_requests", stream.start_requests());
    json_add_int(json, "stop_requests", stream.stop_requests());
    json_add_int(json, "command_deferred", stream.command_deferred());
    json_add_int(json, "command_errors", stream.command_errors());
    if (stream.last_notification_ms()) {
        json_add_int(json, "last_age_ms",
                     millis() - stream.last_notification_ms());
    } else {
        json += ",\"last_age_ms\":null";
    }
    json_add_string(json, "start_time", stream.last_start_time().c_str());
    json_add_string(json, "params", stream.params_json().c_str());
    json += ",\"consumer_slots\":[";
    bool first_consumer = true;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        StreamConsumerHandle handle = static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;
        if (!first_consumer) json += ',';
        first_consumer = false;
        json += "{";
        json_add_bool(json, "active", true, false);
        json_add_int(json, "source",
                     static_cast<unsigned>(stream.consumer_source(handle)));
        json_add_int(json, "queued", stream.consumer_queue_count(handle));
        json_add_int(json, "drops", stream.consumer_queue_drops(handle));
        json += '}';
    }
    json += "]}";
}

void WebUI::build_config_json(LargeTextBuffer &json,
                              const char *section) const {
    const AppConfigData &cfg = config_service_->data();
    json = "{";
    bool comma = false;

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        if (!web_config_section_includes(section, field.group)) continue;

        String value;
        if (!app_config_field_get_raw_value(cfg, field, value)) continue;

        if (app_config_field_is_secret(field)) {
            char set_key[40];
            snprintf(set_key, sizeof(set_key), "%s_set", field.key);
            json_add_bool(json, set_key, value.length() > 0, comma);
            comma = true;
            json_add_string(json, field.key, "");
            comma = true;
            continue;
        }

        switch (field.type) {
            case AppConfigFieldType::Bool:
                json_add_bool(json, field.key, value == "1", comma);
                comma = true;
                break;
            case AppConfigFieldType::UInt16:
                json_add_int(json, field.key, strtol(value.c_str(), nullptr,
                                                     10), comma);
                comma = true;
                break;
            case AppConfigFieldType::String:
            case AppConfigFieldType::Enum:
            case AppConfigFieldType::LogLevel:
                json_add_string(json, field.key, value.c_str(), comma);
                comma = true;
                break;
            case AppConfigFieldType::Secret:
                break;
        }
    }
    json += '}';
}

void WebUI::build_config_schema_json(LargeTextBuffer &json) const {
    json = "{\"groups\":[";

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    AppConfigGroup current_group = AppConfigGroup::Device;
    bool group_open = false;
    bool first_group = true;
    bool first_field = true;

    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        if (!group_open || field.group != current_group) {
            if (group_open) json += "]}";
            if (!first_group) json += ',';
            first_group = false;
            current_group = field.group;
            json += "{";
            json_add_string(json, "id", app_config_group_id(current_group),
                            false);
            json_add_string(json, "label",
                            app_config_group_label(current_group));
            json += ",\"fields\":[";
            group_open = true;
            first_field = true;
        }

        if (!first_field) json += ',';
        first_field = false;
        json += "{";
        json_add_string(json, "key", field.key, false);
        json_add_string(json, "label", field.label);
        json_add_string(json, "type",
                        web_config_field_type_name(field.type));
        json_add_bool(json, "secret", app_config_field_is_secret(field));
        json_add_bool(json, "provisionable",
                      (field.flags & AC_CONFIG_FIELD_PROVISIONABLE) != 0);
        if (field.help && field.help[0]) {
            json_add_string(json, "help", field.help);
        }
        append_config_schema_enum(json, field);
        json += "}";
    }

    if (group_open) json += "]}";
    json += "]}";
}

void WebUI::build_wifi_json(LargeTextBuffer &json) const {
    json = "{";
    json_add_string(json, "state", wifi_manager_->state_name(), false);
    json_add_string(json, "ssid", wifi_manager_->sta_ssid().c_str());
    json_add_int(json, "active", wifi_manager_->active_profile_index());
    json_add_string(json, "ip", wifi_manager_->ip().toString().c_str());
    char bssid_text[AC_WIFI_BSSID_TEXT_MAX];
    wifi_manager_->bssid(bssid_text, sizeof(bssid_text));
    json_add_string(json, "bssid", bssid_text);
    json_add_int(json, "rssi", wifi_manager_->rssi());
    json_add_int(json, "channel", wifi_manager_->channel());
    json_add_bool(json, "roam", wifi_manager_->roaming_enabled());
    json += ",\"profiles\":[";
    for (size_t i = 0; i < wifi_manager_->profile_count(); ++i) {
        const WifiProfile &profile = wifi_manager_->profile(i);
        if (i) json += ',';
        json += "{";
        json_add_string(json, "ssid", profile.ssid.c_str(), false);
        json_add_bool(json, "open", profile.password.length() == 0);
        json += '}';
    }
    json += "]}";
}

void WebUI::publish_snapshots(bool force,
                              bool realtime_active,
                              PollCheckpoint checkpoint) {
    const uint32_t now = millis();
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return;
    const bool periodic_due =
        static_cast<int32_t>(now - last_snapshot_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    uint16_t rebuild_mask = snapshots_dirty_mask_;
    if (force || !snapshots_ready_) {
        rebuild_mask = SNAPSHOT_ALL;
    } else if (periodic_due) {
        rebuild_mask |= SNAPSHOT_PERIODIC;
    }

    if (!force && snapshots_ready_ && realtime_active) {
        rebuild_mask &= SNAPSHOT_STATUS | SNAPSHOT_STREAM;
    }

    if (!rebuild_mask) {
        xSemaphoreGive(cache_mutex_);
        return;
    }

    if (rebuild_mask & SNAPSHOT_STATUS) {
        build_status_json(cached_status_json_, checkpoint);
        if (checkpoint) checkpoint("web_ui.snapshots.status");
    }
    if (rebuild_mask & SNAPSHOT_STREAM) {
        build_stream_json(cached_stream_json_);
        if (checkpoint) checkpoint("web_ui.snapshots.stream");
    }
    if (rebuild_mask & SNAPSHOT_CONFIG) {
        if (checkpoint) checkpoint("web_ui.snapshots.config");
    }
    if (rebuild_mask & SNAPSHOT_WIFI) {
        build_wifi_json(cached_wifi_json_);
        if (checkpoint) checkpoint("web_ui.snapshots.wifi");
    }
    if (rebuild_mask & SNAPSHOT_OXIMETRY_SENSORS) {
        build_oximetry_sensors_json(cached_oximetry_sensors_json_);
        if (checkpoint) checkpoint("web_ui.snapshots.oximetry_sensors");
    }
    if (rebuild_mask & SNAPSHOT_CONFIG) {
        cached_http_auth_required_ =
            network_auth_required(config_service_->data());
        cached_http_user_ = config_service_->data().http_user;
        cached_http_password_ = config_service_->data().http_password;
        cached_auth_whitelist_ = config_service_->data().auth_whitelist;
    }

    snapshots_ready_ = true;
    snapshots_dirty_mask_ &= ~rebuild_mask;
    if (force || periodic_due || (rebuild_mask & SNAPSHOT_PERIODIC)) {
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
        case WebCommandConfigUpdate:
            execute_config_update(command.body);
            break;
        case WebCommandWifiUpdate:
            execute_wifi_update(command.body);
            break;
        case WebCommandTimeAction:
            execute_time_action(command.text);
            break;
        case WebCommandTherapyAction:
            execute_therapy_action(command.text);
            break;
        case WebCommandOximetryAction:
            execute_oximetry_action(command.text, command.body);
            break;
        default:
            break;
    }
}

void WebUI::execute_console_line(const std::string &line) {
    String command(line.c_str());
    command.trim();
    if (!command.length()) return;
    if (!console_ctx_) return;

    StringPrint capture(AC_WEB_CONSOLE_COMMAND_OUTPUT_MAX);
    web_console_.execute_line(command, capture, *console_ctx_);
    String entry = "> ";
    entry += command;
    entry += "\n";
    entry += capture.text();
    append_console_log(entry);
}

void WebUI::execute_config_update(const std::string &body) {
    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) return;

    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull() || !config_service_->begin_transaction()) return;

    for (JsonPairConst pair : root) {
        const char *key = pair.key().c_str();
        const AppConfigFieldDescriptor *field = app_config_find_field(key);
        if (!field) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=unknown\n",
                      key ? key : "<null>");
            continue;
        }

        String value;
        if (!web_config_value_text(pair.value(), *field, value)) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=bad_type\n",
                      field->key);
            continue;
        }

        const ConfigFieldUpdate update =
            config_service_->set_transaction_value(field->key, value, true);
        if (!update.accepted()) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=invalid_value\n",
                      field->key);
        }
    }

    const ConfigTransactionResult result =
        config_service_->commit_transaction();
    if (!result.persisted) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] failed to persist one or more config values\n");
    }
    if (result.changed_fields) mark_snapshots_dirty(SNAPSHOT_ALL);
}

void WebUI::execute_wifi_update(const std::string &body) {
    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) return;
    String action;
    json_get_string(doc, "action", action);
    bool changed = false;
    if (action == "add" || action == "set") {
        String ssid;
        String pass;
        json_get_string(doc, "ssid", ssid);
        json_get_string(doc, "pass", pass);
        if (action == "set") changed = wifi_manager_->configure_sta(ssid, pass);
        else changed = wifi_manager_->add_profile(ssid, pass, !pass.length());
    } else if (action == "remove" && doc["index"].is<int>()) {
        changed = wifi_manager_->remove_profile(doc["index"].as<int>());
    } else if (action == "clear") {
        wifi_manager_->clear_sta_config();
        changed = true;
    } else if (action == "reconnect") {
        changed = wifi_manager_->reconnect();
    }
    if (changed) mark_snapshots_dirty(SNAPSHOT_STATUS | SNAPSHOT_WIFI);
}

void WebUI::execute_time_action(const std::string &action) {
    if (action == "ntp_sync") {
        time_sync_service_->force_ntp_sync();
    } else if (action == "sync_to_resmed") {
        time_sync_service_->request_push_esp_to_resmed(RpcSource::HttpApi);
    } else if (action == "sync_from_resmed") {
        time_sync_service_->request_pull_resmed_to_esp(RpcSource::HttpApi);
    } else if (action == "retry_resmed_push") {
        time_sync_service_->reset_resmed_push();
    }
    mark_snapshots_dirty(SNAPSHOT_STATUS);
}

void WebUI::execute_therapy_action(const std::string &action) {
    As11TherapyTarget target = As11TherapyTarget::None;
    if (action == "start") target = As11TherapyTarget::Running;
    if (action == "stop" || action == "standby") {
        target = As11TherapyTarget::Standby;
    }
    if (target == As11TherapyTarget::None) return;

    (void)device_->request_therapy(*rpc_, target, RpcSource::HttpApi,
                                   millis());
    mark_snapshots_dirty(SNAPSHOT_STATUS);
}

void WebUI::execute_oximetry_action(const std::string &action,
                                    const std::string &body) {
    if (!oximetry_manager_) return;
    if (action == "enable") {
        (void)config_service_->set_value("oxi_en", "1", false);
    } else if (action == "disable") {
        (void)config_service_->set_value("oxi_en", "0", false);
    } else if (action == "pair") {
        (void)config_service_->set_value("oxi_en", "1", false);
        oximetry_manager_->request_pairing(true);
    } else if (action == "pair_stop") {
        oximetry_manager_->request_pairing(false);
    } else if (action == "forget") {
        oximetry_manager_->forget_bonds();
    } else if (action == "advertise_start") {
        oximetry_manager_->request_advertising(true);
    } else if (action == "advertise_stop") {
        oximetry_manager_->request_advertising(false);
    } else if (action == "sensor_scan") {
        oximetry_manager_->request_sensor_scan();
    } else if (action == "sensor_disconnect") {
        oximetry_manager_->request_sensor_disconnect();
    } else if (action == "sensor_connect" ||
               action == "sensor_forget" ||
               action == "sensor_autoconnect") {
        JsonDocument doc;
        if (deserializeJson(doc, body.c_str())) return;
        String target;
        String addr;
        String name;
        json_get_string(doc, "target", target);
        json_get_string(doc, "addr", addr);
        json_get_string(doc, "name", name);
        if (action == "sensor_connect") {
            bool ok = false;
            if (addr.length()) {
                OximetrySensorDevice device;
                strncpy(device.addr, addr.c_str(), sizeof(device.addr) - 1);
                device.addr[sizeof(device.addr) - 1] = 0;
                if (doc["addr_type"].is<uint8_t>()) {
                    device.addr_type = doc["addr_type"].as<uint8_t>();
                }
                if (name.length()) {
                    strncpy(device.name, name.c_str(),
                            sizeof(device.name) - 1);
                    device.name[sizeof(device.name) - 1] = 0;
                }
                if (doc["rssi"].is<int>()) device.rssi = doc["rssi"].as<int>();
                ok = oximetry_manager_->request_sensor_connect_device(device);
            } else {
                ok = oximetry_manager_->request_sensor_connect(target.c_str());
            }
            if (!ok) {
                Log::logf(CAT_OXI, LOG_WARN,
                          "[WEB] sensor connect command rejected target=\"%s\" addr=\"%s\"\n",
                          target.c_str(),
                          addr.c_str());
            }
        } else if (action == "sensor_forget") {
            oximetry_manager_->forget_sensor(addr.c_str());
        } else if (action == "sensor_autoconnect" &&
                   doc["enabled"].is<bool>()) {
            oximetry_manager_->set_sensor_autoconnect(
                addr.c_str(), doc["enabled"].as<bool>());
        }
    }
    mark_snapshots_dirty(SNAPSHOT_STATUS | SNAPSHOT_CONFIG |
                         SNAPSHOT_OXIMETRY_SENSORS);
}

void WebUI::register_routes(HttpRouteModule *const *route_modules,
                            size_t route_module_count) {
    // Static UI and snapshots
    server_->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse(
            200, "text/html", HTML_PAGE_GZ, HTML_PAGE_GZ_SIZE);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server_->on(AsyncURIMatcher::exact("/api/status"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_status_json_);
    });

    server_->on(AsyncURIMatcher::exact("/api/stream"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_stream_json_);
    });

    server_->on(AsyncURIMatcher::exact("/api/live/view"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        send_live_view_state(request);
    });

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

            if (!json_get_string(doc, "cmd", command)) {
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

    // Configuration
    server_->on(AsyncURIMatcher::exact("/api/config"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_config_json(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/config/schema"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_config_schema_json(request);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/config"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_config_update(request);
        },
        nullptr, http_request_body_handler);

    const auto register_config_section =
        [this](const char *path, const char *section) {
            server_->on(AsyncURIMatcher::exact(path), HTTP_GET,
                        [this, section](AsyncWebServerRequest *request) {
                send_config_json(request, section);
            });

            server_->on(
                AsyncURIMatcher::exact(path), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
                    send_config_update(request);
                },
                nullptr, http_request_body_handler);
        };

    register_config_section("/api/config/device", "device");
    register_config_section("/api/config/network", "network");
    register_config_section("/api/config/access", "access");
    register_config_section("/api/config/ota", "ota");
    register_config_section("/api/config/logging", "logging");
    register_config_section("/api/config/time", "time");
    register_config_section("/api/config/oximetry", "oximetry");
    register_config_section("/api/config/smb", "smb");
    register_config_section("/api/config/sleephq", "sleephq");

    server_->on(
        AsyncURIMatcher::exact("/api/time"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            String action;
            json_get_string(doc, "action", action);
            WebCommand queued;
            queued.kind = WebCommandTimeAction;
            queued.text = action.c_str();
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, http_request_body_handler);

    server_->on(
        AsyncURIMatcher::exact("/api/oximetry"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            String action;
            if (!json_get_string(doc, "action", action)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing action\"}");
                return;
            }
            action.trim();
            WebCommand queued;
            queued.kind = WebCommandOximetryAction;
            queued.text = action.c_str();
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, http_request_body_handler);

    server_->on(AsyncURIMatcher::exact("/api/oximetry/sensors"), HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_cached(request, cached_oximetry_sensors_json_);
        });

    server_->on(AsyncURIMatcher::exact("/api/wifi"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_wifi_json_);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/wifi"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand queued;
            queued.kind = WebCommandWifiUpdate;
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, http_request_body_handler);

    server_->on(
        AsyncURIMatcher::exact("/api/therapy"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            String action;
            json_get_string(doc, "action", action);
            if (action != "start" && action != "stop" &&
                action != "standby") {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"unknown action\"}");
                return;
            }
            WebCommand queued;
            queued.kind = WebCommandTherapyAction;
            queued.text = action.c_str();
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, http_request_body_handler);

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
