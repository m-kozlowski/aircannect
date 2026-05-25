#include "web_ui.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>

#include "auth_utils.h"
#include "app_config_update.h"
#include "as11_rpc.h"
#include "debug_log.h"
#include "json_util.h"
#include "memory_manager.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_manager.h"
#include "storage_writer.h"
#include "system_status_snapshot.h"
#include "string_print.h"
#include "version.h"
#include "web_ui_html.h"

namespace aircannect {

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

namespace {

const char *web_command_name(uint8_t kind) {
    switch (kind) {
        case WebCommandConsoleLine: return "console_line";
        case WebCommandConsoleClear: return "console_clear";
        case WebCommandConfigUpdate: return "config_update";
        case WebCommandWifiUpdate: return "wifi_update";
        case WebCommandTimeAction: return "time_action";
        case WebCommandSettingsRefresh: return "settings_refresh";
        case WebCommandSettingsUpdate: return "settings_update";
        case WebCommandTherapyAction: return "therapy_action";
        case WebCommandOximetryAction: return "oximetry_action";
        case WebCommandResmedOtaInit: return "resmed_ota_init";
        case WebCommandResmedOtaBlock: return "resmed_ota_block";
        case WebCommandResmedOtaCheck: return "resmed_ota_check";
        case WebCommandResmedOtaApply: return "resmed_ota_apply";
        case WebCommandResmedOtaAbort: return "resmed_ota_abort";
        case WebCommandResmedOtaStartStaged: return "resmed_ota_start_staged";
        default: return "unknown";
    }
}

void release_request_body(AsyncWebServerRequest *request) {
    if (!request || !request->_tempObject) return;
    Memory::free(request->_tempObject);
    request->_tempObject = nullptr;
}

void handle_body(AsyncWebServerRequest *request,
                 uint8_t *data,
                 size_t len,
                 size_t index,
                 size_t total) {
    if (index == 0) {
        release_request_body(request);
        if (total > AC_WEB_MAX_POST_BODY) return;
        request->_tempObject =
            Memory::calloc_large(AC_WEB_MAX_POST_BODY + 1, sizeof(char));
    }
    char *body = static_cast<char *>(request->_tempObject);
    if (!body || index + len > AC_WEB_MAX_POST_BODY) return;
    memcpy(body + index, data, len);
    body[index + len] = 0;
}

std::string take_request_body(AsyncWebServerRequest *request) {
    std::string body;
    if (request && request->_tempObject) {
        body = static_cast<const char *>(request->_tempObject);
    }
    release_request_body(request);
    return body;
}

bool json_get_string(JsonDocument &doc, const char *key, String &out) {
    if (!doc[key].is<const char *>()) return false;
    out = doc[key].as<const char *>();
    return true;
}

bool parse_body_copy(AsyncWebServerRequest *request,
                     JsonDocument &doc,
                     std::string &body) {
    body = take_request_body(request);
    if (body.empty()) return false;
    return !deserializeJson(doc, body.c_str());
}

int request_profile_mode_arg(AsyncWebServerRequest *request) {
    if (!request) return -1;
    const char *arg_name = nullptr;
    if (request->hasArg("profile_mode")) {
        arg_name = "profile_mode";
    } else if (request->hasArg("mode")) {
        arg_name = "mode";
    } else {
        return -1;
    }
    return as11_mode_index_from_value(
        std::string(request->arg(arg_name).c_str()));
}

int active_settings_mode(const RpcArbiter *arbiter) {
    if (!arbiter) return -1;
    int mode = arbiter->as11_settings().mode_index();
    if (mode < 0) {
        mode = as11_mode_index_from_value(
            arbiter->as11_state().active_therapy_profile());
    }
    return mode;
}

bool request_size_arg(AsyncWebServerRequest *request,
                      const char *name,
                      size_t &out) {
    out = 0;
    if (!request || !name || !request->hasArg(name)) return false;
    const String value = request->arg(name);
    char *end = nullptr;
    const unsigned long long parsed =
        strtoull(value.c_str(), &end, 10);
    if (!end || *end != 0 || parsed == 0 ||
        parsed > AC_RESMED_OTA_MAX_FILE_BYTES) {
        return false;
    }
    out = static_cast<size_t>(parsed);
    return true;
}

String settings_placeholder_json(bool refresh_queued) {
    String json = "{";
    json_add_bool(json, "valid", false, false);
    json_add_bool(json, "refresh_queued", refresh_queued);
    json_add_int(json, "pending_count", 0);
    json_add_string(json, "last_write_status", "");
    json += ",\"last_write_age_ms\":null";
    json += ",\"age_ms\":null";
    json += ",\"settings\":[]}";
    return json;
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

const char *setting_kind_name(As11SettingKind kind) {
    switch (kind) {
        case As11SettingKind::Number: return "number";
        case As11SettingKind::Enum: return "enum";
        case As11SettingKind::Bool: return "bool";
        case As11SettingKind::Text: return "text";
        default: return "text";
    }
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
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(value));
    char *end = buf + strlen(buf);
    while (end > buf && end[-1] == '0') *--end = 0;
    if (end > buf && end[-1] == '.') *--end = 0;
    json += buf;
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
    for (size_t i = 0; i < series.count; ++i) {
        if (i) json += ',';
        if (!series.valid[i]) {
            json += "null";
        } else {
            append_json_float_value(json, series.values[i]);
        }
    }
    json += ']';
}

template <typename JsonOut>
void build_ota_json(JsonOut &json, const OtaManagerStatus &ota) {
    json = "{";
    json_add_string(json, "version", aircannect_version(), false);
    json_add_bool(json, "arduino_started", ota.arduino_started);
    json_add_bool(json, "http_active", ota.http_active);
    json_add_bool(json, "http_ready", ota.http_ready);
    json_add_bool(json, "reboot_pending", ota.reboot_pending);
    json_add_bool(json, "auth_enabled", ota.auth_enabled);
    json_add_int(json, "arduino_port", ota.arduino_port);
    json_add_int(json, "bytes", static_cast<long>(ota.bytes));
    json_add_int(json, "total_size", static_cast<long>(ota.total_size));
    json_add_int(json, "progress", ota.progress_percent);
    json_add_string(json, "method", ota.method.c_str());
    json_add_string(json, "partition", ota.partition.c_str());
    json_add_string(json, "last_error", ota.last_error.c_str());
    json += '}';
}

template <typename JsonOut>
void build_resmed_ota_json(JsonOut &json, const ResmedOtaManager &ota) {
    const ResmedOtaStatus status = ota.status();
    json = "{";
    json_add_string(json, "phase", ota.phase_name(), false);
    json_add_bool(json, "active", ota.active());
    json_add_bool(json, "waiting", status.waiting);
    json_add_int(json, "total_size", static_cast<long>(status.total_size));
    json_add_int(json, "uploaded_bytes",
                 static_cast<long>(status.uploaded_bytes));
    json_add_int(json, "xfer_block_size",
                 static_cast<long>(status.xfer_block_size));
    json_add_int(json, "progress", status.progress_percent);
    json_add_string(json, "filename", status.filename.c_str());
    json_add_string(json, "expected_sha256",
                    status.expected_sha256.c_str());
    json_add_string(json, "computed_sha256",
                    status.computed_sha256.c_str());
    json_add_string(json, "apply_mode", status.apply_mode.c_str());
    json_add_string(json, "input_type", status.input_type.c_str());
    json_add_string(json, "target", status.target.c_str());
    json_add_string(json, "staged_path", status.staged_path.c_str());
    json_add_string(json, "last_result", status.last_result.c_str());
    json_add_string(json, "last_error", status.last_error.c_str());
    json += '}';
}

}  // namespace

bool WebUI::begin(RpcArbiter &arbiter,
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
                  uint16_t port) {
    if (started_) return true;
    stop();
    arbiter_ = &arbiter;
    wifi_manager_ = &wifi_manager;
    tcp_bridge_ = &tcp_bridge;
    app_config_ = &app_config;
    time_sync_service_ = &time_sync_service;
    ota_manager_ = &ota_manager;
    resmed_ota_manager_ = &resmed_ota_manager;
    session_manager_ = &session_manager;
    sink_manager_ = &sink_manager;
    oximetry_manager_ = &oximetry_manager;
    console_ctx_ = &console_ctx;

    command_queue_ = xQueueCreate(AC_WEB_COMMAND_QUEUE_DEPTH,
                                  sizeof(WebCommand *));
    cache_mutex_ = xSemaphoreCreateMutex();
    sse_mutex_ = xSemaphoreCreateMutex();
    if (!command_queue_ || !cache_mutex_ || !sse_mutex_) {
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
    register_routes();
    publish_snapshots(true);
    server_->begin();
    started_ = true;
    Log::logf(CAT_GENERAL, LOG_INFO, "[WEB] listening on port %u\n", port);
    return true;
}

void WebUI::reserve_cached_json() {
    cached_status_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    cached_stream_json_.reserve(AC_WEB_STREAM_JSON_RESERVE);
    cached_config_json_.reserve(AC_WEB_CONFIG_JSON_RESERVE);
    cached_wifi_json_.reserve(AC_WEB_WIFI_JSON_RESERVE);
    cached_oximetry_sensors_json_.reserve(2048);
    cached_ota_json_.reserve(AC_WEB_OTA_JSON_RESERVE);
    cached_resmed_ota_json_.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE);
    cached_settings_json_.reserve(AC_WEB_SETTINGS_JSON_RESERVE);
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
    out.config = capture(cached_config_json_);
    out.wifi = capture(cached_wifi_json_);
    out.oximetry_sensors = capture(cached_oximetry_sensors_json_);
    out.ota = capture(cached_ota_json_);
    out.resmed_ota = capture(cached_resmed_ota_json_);
    out.settings = capture(cached_settings_json_);
    out.live = capture(live_json_);
    out.console_log_length = console_log_length_;
    xSemaphoreGive(cache_mutex_);
    return out;
}

void WebUI::stop() {
    if (sink_manager_) sink_manager_->set_live_chart_enabled(false);
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

    if (command_queue_) {
        WebCommand *command = nullptr;
        while (xQueueReceive(command_queue_, &command, 0) == pdTRUE) {
            delete command;
            command = nullptr;
        }
        vQueueDelete(command_queue_);
        command_queue_ = nullptr;
    }

    if (sse_mutex_) {
        if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (SseClientRef &ref : sse_clients_) ref = {};
            xSemaphoreGive(sse_mutex_);
        }
        vSemaphoreDelete(sse_mutex_);
        sse_mutex_ = nullptr;
    } else {
        for (SseClientRef &ref : sse_clients_) ref = {};
    }

    if (cache_mutex_) {
        vSemaphoreDelete(cache_mutex_);
        cache_mutex_ = nullptr;
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
    last_snapshot_ms_ = 0;
    last_sse_push_ms_ = 0;
    sse_push_requested_ = false;
    started_ = false;
}

void WebUI::poll() {
    if (!started_) return;
    drain_commands();
    enforce_sse_limits();
    poll_live_stream();
    publish_snapshots(false);

    if (!events_ || events_->count() == 0) return;
    const bool push_requested = sse_push_requested_;
    if (!push_requested &&
        static_cast<int32_t>(millis() - last_sse_push_ms_) <
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS)) {
        return;
    }
    last_sse_push_ms_ = millis();
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        sse_push_requested_ = push_requested;
        return;
    }
    sse_push_requested_ = false;
    const uint32_t event_id = millis();
    bool sse_backpressure = false;
    if (events_->send(cached_status_json_.c_str(), "status", event_id) !=
        AsyncEventSource::ENQUEUED) {
        sse_backpressure = true;
    }
    if (events_->send(cached_stream_json_.c_str(), "stream", event_id) !=
        AsyncEventSource::ENQUEUED) {
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
        console_json.reserve(payload_len + 128);
        build_console_sse_json(console_json);
        if (console_json.overflowed() ||
            events_->send(console_json.c_str(), "console", event_id) !=
                AsyncEventSource::ENQUEUED) {
            sse_backpressure = true;
        } else {
            note_console_sse_sent();
        }
    }
    xSemaphoreGive(cache_mutex_);
    if (sse_backpressure) {
        sse_enforce_needed_ = true;
        enforce_sse_limits();
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
        const bool slow = worst_pending > AC_WEB_SSE_CLIENT_PENDING_MAX;
        if (!overflow && !slow) {
            sse_enforce_needed_ = false;
            xSemaphoreGive(sse_mutex_);
            return;
        }
        xSemaphoreGive(sse_mutex_);

        if (!drop) return;
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] closing SSE client count=%u pending=%u\n",
                  static_cast<unsigned>(connected_count),
                  static_cast<unsigned>(worst_pending));
        drop->close();
    }
}

size_t WebUI::sse_client_count() {
    if (!sse_mutex_) return 0;
    size_t count = 0;
    if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return events_ ? events_->count() : 0;
    }
    for (SseClientRef &ref : sse_clients_) {
        AsyncEventSourceClient *client = ref.client;
        if (!client) continue;
        if (!client->connected()) {
            ref.client = nullptr;
            ref.connected_ms = 0;
            continue;
        }
        count++;
    }
    xSemaphoreGive(sse_mutex_);
    return count;
}

void WebUI::poll_live_stream() {
    const size_t clients = sse_client_count();
    if (sink_manager_) sink_manager_->set_live_chart_enabled(clients > 0);
    send_live_batch(millis());
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
    if (sse_client_count() == 0) {
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

    if (events_->send(live_json_.c_str(), "live", now_ms) !=
        AsyncEventSource::ENQUEUED) {
        sse_enforce_needed_ = true;
    }
    live_last_send_ms_ = now_ms;
    sink_manager_->mark_live_chart_sent();
}

void WebUI::handle_event(const RpcEvent &event) {
    if (event.kind == RpcEventKind::BootNotification) {
        mark_snapshots_dirty(SNAPSHOT_STATUS | SNAPSHOT_SETTINGS);
    } else if (event.kind == RpcEventKind::InternalSettingsStateInvalidated) {
        if (cache_mutex_ &&
            xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
            cached_settings_refresh_queued_ = true;
            mark_snapshots_dirty(SNAPSHOT_SETTINGS);
            xSemaphoreGive(cache_mutex_);
        } else {
            mark_snapshots_dirty(SNAPSHOT_SETTINGS);
        }
    } else if (event.kind == RpcEventKind::InternalSettingsStateUpdated) {
        if (cache_mutex_ &&
            xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
            cached_settings_refresh_queued_ = false;
            mark_snapshots_dirty(SNAPSHOT_SETTINGS);
            xSemaphoreGive(cache_mutex_);
        } else {
            mark_snapshots_dirty(SNAPSHOT_SETTINGS);
        }
    } else if (event.kind == RpcEventKind::RpcNotification &&
               json_method_is(event.payload_text(), "EventNotification")) {
        mark_snapshots_dirty(SNAPSHOT_STATUS);
    }

    if (!ManagementConsole::event_has_output(event)) return;

    StringPrint capture;
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
    json = "{";
    json_add_int(json, "seq", console_seq_, false);
    json += ",\"log\":\"";
    append_console_log_json_range(json, console_log_begin_pos(),
                                  console_log_write_pos_);
    json += "\"}";
}

void WebUI::build_console_sse_json(LargeTextBuffer &json) const {
    const uint64_t begin = console_log_begin_pos();
    const uint64_t end = console_log_write_pos_;
    const bool reset = console_sse_reset_pending_ ||
                       console_sse_pos_ < begin ||
                       console_sse_pos_ > end;

    json = "{";
    json_add_int(json, "seq", console_seq_, false);
    if (reset) {
        json_add_bool(json, "reset", true);
        json += ",\"log\":\"";
        append_console_log_json_range(json, begin, end);
    } else {
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
    json.reserve(console_log_length_ + 128);
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
    if (mask & (SNAPSHOT_STATUS | SNAPSHOT_OTA | SNAPSHOT_RESMED_OTA)) {
        request_sse_push();
    }
}

void WebUI::request_sse_push() {
    sse_push_requested_ = true;
}

void WebUI::send_cached_settings(AsyncWebServerRequest *request,
                                 int requested_mode) {
    bool mismatch = false;
    bool has_cached = false;
    bool refresh_queued = false;
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"valid\":false,\"refresh_queued\":true,"
                      "\"settings\":[]}");
        return;
    }
    const int active_mode =
        requested_mode < 0 ? active_settings_mode(arbiter_) : -1;
    const bool explicit_mode_mismatch =
        requested_mode >= 0 &&
        (requested_settings_mode_ != requested_mode ||
         cached_settings_mode_ != requested_mode);
    const bool active_mode_mismatch =
        requested_mode < 0 &&
        (requested_settings_mode_ >= 0 ||
         (active_mode >= 0 && cached_settings_mode_ != active_mode));

    if (explicit_mode_mismatch || active_mode_mismatch) {
        requested_settings_mode_ = requested_mode;
        mark_snapshots_dirty(SNAPSHOT_SETTINGS);
        mismatch = true;
    } else {
        refresh_queued = cached_settings_refresh_queued_;
        has_cached = !refresh_queued && cached_settings_json_.length() > 0;
    }

    if (!mismatch && has_cached) {
        AsyncResponseStream *response =
            request->beginResponseStream("application/json");
        if (!response) {
            xSemaphoreGive(cache_mutex_);
            request->send(503, "application/json",
                          "{\"valid\":false,\"error\":\"response alloc\"}");
            return;
        }
        response->write(
            reinterpret_cast<const uint8_t *>(cached_settings_json_.c_str()),
            cached_settings_json_.length());
        xSemaphoreGive(cache_mutex_);
        request->send(response);
        return;
    }
    xSemaphoreGive(cache_mutex_);

    const String placeholder =
        settings_placeholder_json(mismatch || refresh_queued);
    request->send(200, "application/json", placeholder);
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

bool WebUI::enqueue_command(WebCommand *command) {
    if (!command || !command_queue_) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue unavailable kind=%s\n",
                  command ? web_command_name(command->kind) : "none");
        delete command;
        return false;
    }
    WebCommand *queued = command;
    if (xQueueSend(command_queue_, &queued, 0) != pdTRUE) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue full kind=%s\n",
                  web_command_name(command->kind));
        delete command;
        return false;
    }
    return true;
}

bool WebUI::enqueue_simple_command(uint8_t kind) {
    WebCommand *command = new WebCommand();
    if (!command) return false;
    command->kind = kind;
    return enqueue_command(command);
}

void WebUI::drain_commands() {
    if (!command_queue_) return;
    for (size_t i = 0; i < AC_WEB_COMMANDS_PER_POLL; ++i) {
        WebCommand *command = nullptr;
        if (xQueueReceive(command_queue_, &command, 0) != pdTRUE) break;
        if (!command) continue;
        execute_command(*command);
        delete command;
    }
}

void WebUI::build_status_json(LargeTextBuffer &json) const {
    const SystemStatusSnapshot snap = collect_system_status({
        *arbiter_,
        *wifi_manager_,
        *app_config_,
        *time_sync_service_,
        *ota_manager_,
        *oximetry_manager_,
    });
    const MemoryStatus &mem = snap.memory;
    const StorageStatus &storage = snap.storage;
    const WifiStatusSnapshot &wifi = snap.wifi;
    const As11StatusSnapshot &as11 = snap.as11;
    const OximetryRuntimeStatus &oxi = snap.oximetry;
    const TimeStatusSnapshot &time = snap.time;

    json = "{";
    json_add_string(json, "version", snap.version, false);
    json_add_string(json, "built", snap.built);
    json_add_int(json, "uptime", snap.uptime_s);
    json_add_int(json, "heap", static_cast<long>(mem.heap_free));
    json_add_bool(json, "psram_available", mem.psram_available);
    json_add_int(json, "psram_free", static_cast<long>(mem.psram_free));
    json_add_bool(json, "storage_configured", storage.configured);
    json_add_bool(json, "storage_mounted", storage.mounted);
    json_add_string(json, "storage_type",
                    Storage::type_name(storage.type));
    json_add_string(json, "storage_state",
                    Storage::state_name(storage.state));
    json_add_uint64(json, "storage_total", storage.total_bytes);
    json_add_uint64(json, "storage_used", storage.used_bytes);
    json_add_uint64(json, "storage_free", storage.free_bytes);
    json_add_string_view(json, "wifi_state", wifi.state);
    json_add_string_view(json, "wifi_ssid", wifi.ssid);
    json_add_string(json, "wifi_ip", wifi.ip);
    json_add_string(json, "softap_mode",
                    softap_mode_name(wifi.softap_mode));
    json_add_bool(json, "softap_running", wifi.softap_running);
    json_add_int(json, "wifi_rssi", wifi.rssi);
    json_add_int(json, "wifi_channel", wifi.channel);
    json_add_string(json, "wifi_bssid", wifi.bssid);
    json_add_int(json, "wifi_profile", wifi.active_profile);
    json_add_bool(json, "wifi_roam", wifi.roaming_enabled);
    json_add_bool(json, "wifi_roam_suspended", wifi.roaming_suspended);
    json_add_bool(json, "ota_active", snap.ota_active);
    json_add_string_view(json, "device_name", as11.product_name);
    json_add_string_view(json, "serial", as11.serial_number);
    json_add_string_view(json, "software_id", as11.software_identifier);
    json_add_string(json, "therapy",
                    As11DeviceState::therapy_state_name(as11.therapy_state));
    json_add_string(json, "therapy_pending",
                    As11DeviceState::therapy_target_name(
                        as11.pending_therapy_target));
    json_add_string_view(json, "rop", as11.rop);
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
    const StreamBroker &stream = arbiter_->stream_broker();
    const RpcArbiterStats &stats = arbiter_->stats();
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
    json_add_int(json, "notifications", stats.stream_notifications);
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
    json_add_int(json, "start_requests", stats.stream_start_requests);
    json_add_int(json, "stop_requests", stats.stream_stop_requests);
    json_add_int(json, "command_deferred", stats.stream_command_deferred);
    json_add_int(json, "command_errors", stats.stream_command_errors);
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
        json_add_int(json, "source", stream.consumer_source(handle));
        json_add_int(json, "queued", stream.consumer_queue_count(handle));
        json_add_int(json, "drops", stream.consumer_queue_drops(handle));
        json += '}';
    }
    json += "]}";
}

void WebUI::build_config_json(LargeTextBuffer &json) const {
    const AppConfigData &cfg = app_config_->data();
    json = "{";
    json_add_string(json, "hostname", cfg.hostname.c_str(), false);
    json_add_bool(json, "tcp_enabled", cfg.tcp_bridge_enabled);
    json_add_int(json, "tcp_port", cfg.tcp_bridge_port);
    json_add_string(json, "softap_mode", softap_mode_name(cfg.softap_mode));
    json_add_string(json, "wifi_country", cfg.wifi_country.c_str());
    json_add_string(json, "timezone", cfg.timezone.c_str());
    json_add_bool(json, "resmed_time_sync_enabled",
                  cfg.resmed_time_sync_enabled);
    json_add_bool(json, "oximetry_enabled", cfg.oximetry_enabled);
    json_add_int(json, "oximetry_udp_port", cfg.oximetry_udp_port);
    json_add_string(json, "oximetry_advertise_mode",
                    oximetry_advertise_mode_name(
                        cfg.oximetry_advertise_mode));
    json_add_bool(json, "http_auth_required", network_auth_required(cfg));
    json_add_string(json, "http_user", cfg.http_user.c_str());
    json_add_bool(json, "http_password_set", cfg.http_password.length() > 0);
    json_add_string(json, "http_password", "");
    json_add_string(json, "auth_whitelist", cfg.auth_whitelist.c_str());
    json_add_bool(json, "telnet_enabled", cfg.telnet_console_enabled);
    json_add_int(json, "telnet_port", cfg.telnet_console_port);
    json_add_bool(json, "ota_password_set", cfg.ota_password.length() > 0);
    json_add_string(json, "ota_password", "");
    json += '}';
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

void WebUI::build_settings_json(LargeTextBuffer &json,
                                int requested_mode,
                                bool refresh_queued) const {
    const As11SettingsState &state = arbiter_->as11_settings();
    const As11DeviceState &as11 = arbiter_->as11_state();
    int active_mode = state.mode_index();
    if (active_mode < 0) {
        active_mode = as11_mode_index_from_value(
            as11.active_therapy_profile());
    }
    int profile_mode = requested_mode >= 0 ? requested_mode : active_mode;
    const uint16_t supported_modes = state.supported_mode_mask();
    if (profile_mode >= 0 && supported_modes &&
        !(supported_modes & (1u << profile_mode))) {
        profile_mode = active_mode;
    }

    json = "{";
    json_add_bool(json, "valid", state.valid(), false);
    json_add_bool(json, "refresh_queued", refresh_queued);
    json_add_int(json, "supported_mode_mask", supported_modes);
    json_add_int(json, "pending_count",
                 static_cast<long>(state.pending_count()));
    json_add_string(json, "last_write_status",
                    state.last_write_status().c_str());
    if (state.last_write_ms()) {
        json_add_int(json, "last_write_age_ms",
                     millis() - state.last_write_ms());
    } else {
        json += ",\"last_write_age_ms\":null";
    }
    if (state.valid()) {
        json_add_int(json, "age_ms", millis() - state.updated_ms());
    } else {
        json += ",\"age_ms\":null";
    }
    json += ",\"settings\":[";
    size_t emitted = 0;
    for (size_t i = 0; i < as11_setting_count(); ++i) {
        const As11SettingDef &def = as11_setting(i);
        if (!state.setting_visible(i, profile_mode)) continue;
        if (!as11_setting_readable_via_rpc(def)) continue;

        const bool is_therapy_mode = strcmp(def.name, "TherapyMode") == 0;
        std::string value =
            state.value(i, is_therapy_mode ? active_mode : profile_mode);
        const bool available = !value.empty() || state.pending(i);
        const bool pending = state.pending(i);
        const bool writable = as11_setting_writable_via_rpc(def, profile_mode);
        if (!available && !pending) continue;

        if (emitted++) json += ',';
        json += "{";
        json_add_string(json, "name", def.name, false);
        json_add_string(json, "value", value.c_str());
        if (!available) json_add_bool(json, "available", false);
        if (!writable) json_add_bool(json, "writable", false);
        if (pending) {
            json_add_bool(json, "pending", true);
            json_add_string(json, "pending_value",
                            state.pending_value(i).c_str());
            json_add_int(json, "pending_age_ms",
                         millis() - state.pending_since_ms(i));
        }
        json += '}';
    }
    json += "]}";
}

void WebUI::build_settings_catalog_json(LargeTextBuffer &json) const {
    json = "{\"settings\":[";
    size_t emitted = 0;
    for (size_t i = 0; i < as11_setting_count(); ++i) {
        const As11SettingDef &def = as11_setting(i);
        if (!as11_setting_readable_via_rpc(def)) continue;

        if (emitted++) json += ',';
        json += "{";
        json_add_string(json, "name", def.name, false);
        json_add_string(json, "label", def.label);
        json_add_string(json, "group", def.group);
        json_add_string(json, "kind", setting_kind_name(def.kind));
        json_add_int(json, "modes", def.mode_mask);
        json_add_float(json, "min", def.min_value);
        json_add_float(json, "max", def.max_value);
        json_add_float(json, "step", def.step);
        json_add_int(json, "scale_div", def.scale_div);
        json_add_int(json, "decimals", def.decimals);
        if (def.options && def.option_count) {
            json += ",\"options\":[";
            size_t options_emitted = 0;
            for (uint8_t opt_index = 0; opt_index < def.option_count;
                 ++opt_index) {
                if (options_emitted++) json += ',';
                json += "{";
                json_add_int(json, "value", opt_index, false);
                json_add_string(json, "label", def.options[opt_index]);
                json += "}";
            }
            json += ']';
        }
        json += '}';
    }
    json += "]}";
}

void WebUI::publish_snapshots(bool force) {
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

    if (!rebuild_mask) {
        xSemaphoreGive(cache_mutex_);
        return;
    }

    if (rebuild_mask & SNAPSHOT_STATUS) build_status_json(cached_status_json_);
    if (rebuild_mask & SNAPSHOT_STREAM) build_stream_json(cached_stream_json_);
    if (rebuild_mask & SNAPSHOT_CONFIG) build_config_json(cached_config_json_);
    if (rebuild_mask & SNAPSHOT_WIFI) build_wifi_json(cached_wifi_json_);
    if (rebuild_mask & SNAPSHOT_OXIMETRY_SENSORS) {
        build_oximetry_sensors_json(cached_oximetry_sensors_json_);
    }
    if (rebuild_mask & SNAPSHOT_OTA) {
        build_ota_json(cached_ota_json_, ota_manager_->status());
    }
    if (rebuild_mask & SNAPSHOT_RESMED_OTA) {
        build_resmed_ota_json(cached_resmed_ota_json_, *resmed_ota_manager_);
    }
    if (rebuild_mask & SNAPSHOT_SETTINGS) {
        const int requested_mode = requested_settings_mode_;
        const bool refresh_queued = cached_settings_refresh_queued_;
        int published_settings_mode = requested_mode;
        if (published_settings_mode < 0) {
            published_settings_mode = active_settings_mode(arbiter_);
        }

        build_settings_json(cached_settings_json_,
                            requested_mode,
                            refresh_queued);
        cached_settings_mode_ = published_settings_mode;
    }
    if (rebuild_mask & SNAPSHOT_CONFIG) {
        cached_http_auth_required_ = network_auth_required(app_config_->data());
        cached_http_user_ = app_config_->data().http_user;
        cached_http_password_ = app_config_->data().http_password;
        cached_auth_whitelist_ = app_config_->data().auth_whitelist;
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
        case WebCommandSettingsRefresh:
            arbiter_->request_as11_settings_refresh();
            mark_snapshots_dirty(SNAPSHOT_SETTINGS);
            break;
        case WebCommandSettingsUpdate:
            execute_settings_update(command.body);
            break;
        case WebCommandTherapyAction:
            execute_therapy_action(command.text);
            break;
        case WebCommandOximetryAction:
            execute_oximetry_action(command.text, command.body);
            break;
        case WebCommandResmedOtaInit:
        case WebCommandResmedOtaBlock:
        case WebCommandResmedOtaCheck:
        case WebCommandResmedOtaApply:
        case WebCommandResmedOtaAbort:
        case WebCommandResmedOtaStartStaged:
            execute_resmed_ota_command(command);
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

    StringPrint capture;
    web_console_.execute_line(command, capture, *console_ctx_);
    String entry = "> ";
    entry += command;
    entry += "\n";
    entry += capture.text();
    append_console_log(entry);
}

void WebUI::execute_config_update(const std::string &body) {
    AppConfigUpdateResult result;
    const bool parsed = apply_web_config_update(
        *app_config_, body,
        {wifi_manager_->mode_state(), wifi_manager_->has_sta_config()},
        result);
    if (!parsed) return;
    if (!result.persisted) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] failed to persist one or more config values\n");
    }
    apply_config_runtime_effects(result, *app_config_, *wifi_manager_,
                                 *ota_manager_);
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

void WebUI::execute_settings_update(const std::string &body) {
    const As11SettingsState &state = arbiter_->as11_settings();
    const As11DeviceState &as11 = arbiter_->as11_state();
    int mode = state.mode_index();
    if (mode < 0) {
        mode = as11_mode_index_from_value(as11.active_therapy_profile());
    }
    size_t accepted = 0;
    std::string params = as11_build_set_params_from_json(body, mode, accepted);
    if (!accepted) return;
    if (arbiter_->send_request("Set", params, RpcSource::HttpApi)) {
        arbiter_->request_as11_settings_refresh();
        mark_snapshots_dirty(SNAPSHOT_SETTINGS);
    }
}

void WebUI::execute_therapy_action(const std::string &action) {
    const char *method = nullptr;
    if (action == "start") method = "EnterTherapy";
    if (action == "stop" || action == "standby") method = "EnterStandby";
    if (!method) return;
    arbiter_->send_request(method, "", RpcSource::HttpApi);
    mark_snapshots_dirty(SNAPSHOT_STATUS);
}

void WebUI::execute_oximetry_action(const std::string &action,
                                    const std::string &body) {
    if (!oximetry_manager_) return;
    if (action == "enable") {
        oximetry_manager_->set_enabled(true);
    } else if (action == "disable") {
        oximetry_manager_->set_enabled(false);
    } else if (action == "pair") {
        oximetry_manager_->set_enabled(true);
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
                          "[OXI] Web sensor connect command rejected target=\"%s\" addr=\"%s\"\n",
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

void WebUI::execute_resmed_ota_command(const WebCommand &command) {
    if (command.kind == WebCommandResmedOtaCheck) {
        resmed_ota_manager_->request_check();
        mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
        return;
    }
    if (command.kind == WebCommandResmedOtaAbort) {
        resmed_ota_manager_->abort("aborted");
        mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
        return;
    }
    if (command.kind == WebCommandResmedOtaStartStaged) {
        resmed_ota_manager_->start_staged_upload();
        mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, command.body.c_str())) return;
    if (command.kind == WebCommandResmedOtaInit) {
        if (!doc["size"].is<int>()) return;
        String sha;
        String filename;
        json_get_string(doc, "sha256", sha);
        json_get_string(doc, "filename", filename);
        const int parsed_size = doc["size"].as<int>();
        if (parsed_size > 0) {
            resmed_ota_manager_->begin_upload(
                static_cast<size_t>(parsed_size), sha, filename);
        }
    } else if (command.kind == WebCommandResmedOtaBlock) {
        if (!doc["offset"].is<int>()) return;
        String data;
        if (!json_get_string(doc, "data", data)) return;
        const int parsed_offset = doc["offset"].as<int>();
        if (parsed_offset >= 0) {
            resmed_ota_manager_->submit_block(
                static_cast<size_t>(parsed_offset), data);
        }
    } else if (command.kind == WebCommandResmedOtaApply) {
        String mode;
        String confirm;
        json_get_string(doc, "mode", mode);
        json_get_string(doc, "confirm", confirm);
        if (mode == "plain") {
            const bool reset =
                doc["reset"].is<bool>() && doc["reset"].as<bool>();
            resmed_ota_manager_->request_apply_plain(reset, confirm);
        } else if (mode == "authenticated") {
            String authentication;
            json_get_string(doc, "authentication", authentication);
            resmed_ota_manager_->request_apply_authenticated(authentication,
                                                            confirm);
        }
    }
    mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
}

void WebUI::register_routes() {
    server_->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse(
            200, "text/html", HTML_PAGE_GZ, HTML_PAGE_GZ_SIZE);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server_->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_status_json_);
    });

    server_->on("/api/stream", HTTP_GET, [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_stream_json_);
    });

    server_->on("/api/console", HTTP_GET, [this](AsyncWebServerRequest *request) {
        send_console_snapshot(request);
    });

    server_->on(
        "/api/console", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
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

            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandConsoleLine;
                queued->text = command.c_str();
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on(
        "/api/console/clear", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_queue_result(request,
                              enqueue_simple_command(WebCommandConsoleClear));
        });

    server_->on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_config_json_);
    });

    server_->on(
        "/api/config", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandConfigUpdate;
                queued->body = body;
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on("/api/ota", HTTP_GET, [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_ota_json_);
    });

    server_->on(
        "/api/ota/upload", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            const bool ok = ota_manager_->finish_http_upload();
            String json;
            json.reserve(AC_WEB_OTA_JSON_RESERVE);
            build_ota_json(json, ota_manager_->status());
            request->send(ok ? 200 : 400, "application/json", json);
        },
        [this](AsyncWebServerRequest *request, const String &filename,
               size_t index, uint8_t *data, size_t len, bool final) {
            (void)final;
            if (index == 0) {
                size_t declared_size = 0;
                if (!request_size_arg(request, "size", declared_size)) {
                    ota_manager_->abort_http_upload("missing_size");
                    return;
                }
                if (!ota_manager_->begin_http_upload(filename, declared_size)) {
                    return;
                }
            }
            ota_manager_->write_http_upload(data, len);
        });

    server_->on("/api/resmed-ota", HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_cached(request, cached_resmed_ota_json_);
        });

    server_->on(
        "/api/resmed-ota/upload", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            const ResmedOtaStatus status = resmed_ota_manager_->status();
            bool ok = status.phase == ResmedOtaPhase::Staging &&
                      resmed_ota_manager_->finish_staged_upload();
            if (ok && !enqueue_simple_command(WebCommandResmedOtaStartStaged)) {
                resmed_ota_manager_->abort("web_queue_full");
                ok = false;
            }
            mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
            String json;
            json.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE);
            build_resmed_ota_json(json, *resmed_ota_manager_);
            request->send(ok ? 200 : 400, "application/json", json);
        },
        [this](AsyncWebServerRequest *request, const String &filename,
               size_t index, uint8_t *data, size_t len, bool final) {
            (void)final;
            if (index == 0) {
                size_t declared_size = 0;
                if (!request_size_arg(request, "size", declared_size)) {
                    resmed_ota_manager_->abort("missing_size");
                    return;
                }
                const String magic =
                    request->hasArg("magic") ? request->arg("magic") : "";
                if (!resmed_ota_manager_->begin_staged_upload(
                        declared_size, filename, magic)) {
                    return;
                }
            }
            resmed_ota_manager_->write_staged_upload(index, data, len);
        });

    server_->on(
        "/api/resmed-ota/init", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            if (!doc["size"].is<int>()) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing size\"}");
                return;
            }
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandResmedOtaInit;
                queued->body = body;
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on(
        "/api/resmed-ota/block", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            if (!doc["offset"].is<int>()) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing offset\"}");
                return;
            }
            String data;
            if (!json_get_string(doc, "data", data)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing data\"}");
                return;
            }
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandResmedOtaBlock;
                queued->body = body;
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on(
        "/api/resmed-ota/check", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_queue_result(request,
                              enqueue_simple_command(WebCommandResmedOtaCheck));
        });

    server_->on(
        "/api/resmed-ota/apply", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandResmedOtaApply;
                queued->body = body;
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on(
        "/api/resmed-ota/abort", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_queue_result(request,
                              enqueue_simple_command(WebCommandResmedOtaAbort));
        });

    server_->on(
        "/api/time", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            String action;
            json_get_string(doc, "action", action);
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandTimeAction;
                queued->text = action.c_str();
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on(
        "/api/oximetry", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
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
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandOximetryAction;
                queued->text = action.c_str();
                queued->body = body;
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on("/api/oximetry/sensors", HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_cached(request, cached_oximetry_sensors_json_);
        });

    server_->on("/api/wifi", HTTP_GET, [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_wifi_json_);
    });

    server_->on(
        "/api/wifi", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandWifiUpdate;
                queued->body = body;
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on("/api/settings-catalog", HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            LargeTextBuffer json;
            json.reserve(AC_WEB_SETTINGS_CATALOG_JSON_RESERVE);
            build_settings_catalog_json(json);
            if (json.overflowed()) {
                request->send(503, "application/json",
                              "{\"ok\":false,\"error\":\"catalog alloc\"}");
                return;
            }
            AsyncResponseStream *response =
                request->beginResponseStream("application/json");
            if (!response) {
                request->send(503, "application/json",
                              "{\"ok\":false,\"error\":\"response alloc\"}");
                return;
            }
            response->write(
                reinterpret_cast<const uint8_t *>(json.c_str()),
                json.length());
            request->send(response);
        });

    server_->on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (request->url() != "/api/settings") {
            request->send(404, "application/json",
                          "{\"ok\":false,\"error\":\"not found\"}");
            return;
        }
        const int mode = request_profile_mode_arg(request);
        if (request->hasArg("refresh")) {
            if (cache_mutex_ &&
                xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                cached_settings_refresh_queued_ = true;
                mark_snapshots_dirty(SNAPSHOT_SETTINGS);
                xSemaphoreGive(cache_mutex_);
            }
            enqueue_simple_command(WebCommandSettingsRefresh);
        }
        send_cached_settings(request, mode);
    });

    server_->on(
        "/api/settings", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandSettingsUpdate;
                queued->body = body;
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->on(
        "/api/therapy", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
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
            WebCommand *queued = new WebCommand();
            if (queued) {
                queued->kind = WebCommandTherapyAction;
                queued->text = action.c_str();
            }
            send_queue_result(request, enqueue_command(queued));
        },
        nullptr, handle_body);

    server_->onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "application/json",
                      "{\"ok\":false,\"error\":\"not found\"}");
    });
}

}  // namespace aircannect
