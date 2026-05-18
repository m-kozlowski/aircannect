#include "web_ui.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth_utils.h"
#include "as11_rpc.h"
#include "debug_log.h"
#include "json_util.h"
#include "memory_manager.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_manager.h"
#include "storage_writer.h"
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
    WebCommandResmedOtaInit,
    WebCommandResmedOtaBlock,
    WebCommandResmedOtaCheck,
    WebCommandResmedOtaApply,
    WebCommandResmedOtaAbort,
};

struct WebCommand {
    uint8_t kind = WebCommandConsoleLine;
    std::string text;
    std::string body;
};

namespace {

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

int request_mode_arg(AsyncWebServerRequest *request) {
    if (!request || !request->hasArg("mode")) return -1;
    return as11_mode_index_from_value(
        std::string(request->arg("mode").c_str()));
}

String settings_placeholder_json(int mode, bool refresh_queued) {
    String json = "{";
    json_add_bool(json, "valid", false, false);
    json_add_bool(json, "refresh_queued", refresh_queued);
    if (mode >= 0) json_add_int(json, "mode", mode);
    else json += ",\"mode\":null";
    json_add_string(json, "mode_name", as11_mode_name(mode));
    json += ",\"active_mode\":null";
    json_add_string(json, "active_mode_name", "");
    json_add_int(json, "pending_count", 0);
    json_add_string(json, "last_write_status", "");
    json += ",\"last_write_age_ms\":null";
    json += ",\"age_ms\":null";
    json += ",\"settings\":[]}";
    return json;
}

String motor_hours(const std::string &iso_duration) {
    if (iso_duration.size() < 4 || iso_duration.rfind("PT", 0) != 0) {
        return "";
    }
    size_t start = 2;
    size_t end = iso_duration.find('S', start);
    if (end == std::string::npos) return "";
    uint64_t seconds = strtoull(
        iso_duration.substr(start, end - start).c_str(), nullptr, 10);
    return String(static_cast<unsigned long>((seconds + 1800) / 3600));
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
                        const WebLiveSeriesBatch &series,
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

void append_live_sample(WebLiveSeriesBatch &series,
                        bool valid,
                        float value,
                        uint32_t &drops) {
    if (series.count >= AC_WEB_LIVE_BATCH_SAMPLES_MAX) {
        drops++;
        return;
    }
    const size_t index = series.count++;
    series.valid[index] = valid ? 1 : 0;
    series.values[index] = valid ? value : 0.0f;
}

bool append_frame_signal(const StreamFrameData &frame,
                         StreamSignalId id,
                         WebLiveSeriesBatch &series,
                         uint32_t &drops,
                         float scale = 1.0f) {
    const StreamSignalSpan *span = frame.find_signal(id);
    if (!span) return false;
    for (uint16_t i = 0; i < span->sample_count; ++i) {
        const size_t index = span->value_offset + i;
        const bool valid =
            index < frame.value_count && frame.value_valid(index);
        append_live_sample(series, valid,
                           valid ? frame.values[index] * scale : 0.0f,
                           drops);
    }
    return true;
}

template <typename JsonOut>
void build_ota_json(JsonOut &json, const OtaManagerStatus &ota) {
    json = "{";
    json_add_bool(json, "arduino_started", ota.arduino_started, false);
    json_add_bool(json, "http_active", ota.http_active);
    json_add_bool(json, "http_ready", ota.http_ready);
    json_add_bool(json, "reboot_pending", ota.reboot_pending);
    json_add_bool(json, "auth_enabled", ota.auth_enabled);
    json_add_int(json, "arduino_port", ota.arduino_port);
    json_add_int(json, "bytes", static_cast<long>(ota.bytes));
    json_add_int(json, "progress", ota.progress_percent);
    json_add_string(json, "method", ota.method.c_str());
    json_add_string(json, "partition", ota.partition.c_str());
    json_add_string(json, "last_error", ota.last_error.c_str());
    json += '}';
}

template <typename JsonOut>
void build_resmed_ota_json(JsonOut &json, const ResmedOtaManager &ota) {
    const ResmedOtaStatus &status = ota.status();
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
    cached_console_json_.reserve(AC_WEB_CONSOLE_LOG_MAX + 128);
    cached_config_json_.reserve(AC_WEB_CONFIG_JSON_RESERVE);
    cached_wifi_json_.reserve(AC_WEB_WIFI_JSON_RESERVE);
    cached_ota_json_.reserve(AC_WEB_OTA_JSON_RESERVE);
    cached_resmed_ota_json_.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE);
    cached_settings_json_.reserve(AC_WEB_SETTINGS_JSON_RESERVE);
    live_json_.reserve(4096);
}

void WebUI::stop() {
    release_live_stream();
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
    live_next_attach_ms_ = 0;
    live_last_send_ms_ = 0;
    live_last_frame_ms_ = 0;
    live_state_dirty_ = true;
    live_last_error_[0] = 0;
    live_json_ = "";
    reset_live_batch();
    snapshots_ready_ = false;
    snapshots_dirty_ = true;
    last_snapshot_ms_ = 0;
    last_sse_push_ms_ = 0;
    started_ = false;
}

void WebUI::poll() {
    if (!started_) return;
    drain_commands();
    enforce_sse_limits();
    poll_live_stream();
    publish_snapshots(false);

    if (!events_ || events_->count() == 0) return;
    if (static_cast<int32_t>(millis() - last_sse_push_ms_) <
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS)) {
        return;
    }
    last_sse_push_ms_ = millis();
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return;
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
        if (events_->send(cached_console_json_.c_str(), "console", event_id) !=
            AsyncEventSource::ENQUEUED) {
            sse_backpressure = true;
        }
        console_sse_seq_ = console_seq_;
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

bool WebUI::live_stream_active() const {
    return arbiter_ &&
           live_stream_handle_ != STREAM_CONSUMER_INVALID &&
           arbiter_->stream_consumer_active(live_stream_handle_);
}

bool WebUI::live_stream_should_run(size_t clients) const {
    if (!arbiter_ || clients == 0) return false;
    const As11TherapyState therapy = arbiter_->as11_state().therapy_state();
    if (therapy == As11TherapyState::Running) return true;
    if (session_manager_ &&
        session_manager_->status().state == SessionState::Active) {
        return true;
    }
    return false;
}

void WebUI::poll_live_stream() {
    const size_t clients = sse_client_count();
    const uint32_t now = millis();
    if (!live_stream_should_run(clients)) {
        if (live_stream_active()) release_live_stream();
        send_live_batch(now);
        return;
    }

    if (!live_stream_active()) {
        attach_live_stream(now);
    }
    drain_live_stream(now);
    send_live_batch(now);
}

void WebUI::attach_live_stream(uint32_t now_ms) {
    if (!arbiter_) return;
    if (static_cast<int32_t>(now_ms - live_next_attach_ms_) < 0) return;
    live_next_attach_ms_ = now_ms + AC_SINK_ATTACH_RETRY_MS;

    const std::string params = build_stream_params(DEFAULT_EDF_STREAM_IDS,
                                                   10,
                                                   50);
    StreamAcquireResult result =
        arbiter_->acquire_stream(params, RpcSource::Sink);
    if (result.status == StreamAcquireStatus::Acquired ||
        result.status == StreamAcquireStatus::AlreadyActive) {
        live_stream_handle_ = result.handle;
        live_last_error_[0] = 0;
        live_state_dirty_ = true;
        snapshots_dirty_ = true;
        return;
    }

    live_attach_failures_++;
    snprintf(live_last_error_, sizeof(live_last_error_), "acquire:%u",
             static_cast<unsigned>(result.status));
    live_state_dirty_ = true;
    snapshots_dirty_ = true;
}

void WebUI::release_live_stream() {
    if (arbiter_ && live_stream_active()) {
        arbiter_->release_stream(live_stream_handle_);
    }
    live_stream_handle_ = STREAM_CONSUMER_INVALID;
    reset_live_batch();
    live_state_dirty_ = true;
    snapshots_dirty_ = true;
}

void WebUI::drain_live_stream(uint32_t now_ms) {
    if (!live_stream_active()) return;

    for (size_t i = 0; i < AC_WEB_LIVE_FRAME_BUDGET; ++i) {
        StreamFrameRef frame;
        if (!arbiter_->next_stream_frame(live_stream_handle_, frame)) break;
        if (!frame) continue;

        append_frame_signal(*frame, StreamSignalId::MaskPressure100Hz,
                            live_pressure_, live_drops_);
        append_frame_signal(*frame, StreamSignalId::PatientFlow100Hz,
                            live_flow_, live_drops_, 60.0f);
        append_frame_signal(*frame, StreamSignalId::Leak50Hz,
                            live_leak_, live_drops_, 60.0f);
        if (!append_frame_signal(*frame,
                                 StreamSignalId::InspiratoryPressure50Hz,
                                 live_inspiratory_pressure_, live_drops_)) {
            append_frame_signal(*frame,
                                StreamSignalId::InspiratoryPressureTwoSecond,
                                live_inspiratory_pressure_, live_drops_);
        }
        if (!append_frame_signal(*frame,
                                 StreamSignalId::ExpiratoryPressure50Hz,
                                 live_expiratory_pressure_, live_drops_)) {
            append_frame_signal(*frame,
                                StreamSignalId::ExpiratoryPressureTwoSecond,
                                live_expiratory_pressure_, live_drops_);
        }
        append_frame_signal(*frame, StreamSignalId::SpO2,
                            live_spo2_, live_drops_);
        append_frame_signal(*frame, StreamSignalId::HeartRate,
                            live_pulse_, live_drops_);

        if (session_manager_) {
            session_manager_->note_stream_frame(*frame, now_ms);
        }
        live_frames_++;
        live_last_frame_ms_ = now_ms;
    }
}

void WebUI::send_live_batch(uint32_t now_ms) {
    if (!events_) return;
    const bool has_samples =
        live_pressure_.count || live_flow_.count || live_leak_.count ||
        live_inspiratory_pressure_.count ||
        live_expiratory_pressure_.count ||
        live_spo2_.count || live_pulse_.count;
    const bool interval_due =
        static_cast<int32_t>(now_ms - live_last_send_ms_) >=
        static_cast<int32_t>(AC_WEB_LIVE_PUSH_INTERVAL_MS);
    const bool heartbeat_due =
        static_cast<int32_t>(now_ms - live_last_send_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    if (has_samples && !interval_due) return;
    if (!has_samples && !live_state_dirty_ && !heartbeat_due) return;
    if (sse_client_count() == 0) {
        reset_live_batch();
        return;
    }

    live_json_ = "{";
    json_add_int(live_json_, "seq", static_cast<long>(++live_seq_), false);
    json_add_bool(live_json_, "active", live_stream_should_run(1));
    json_add_bool(live_json_, "attached", live_stream_active());
    json_add_int(live_json_, "frames", static_cast<long>(live_frames_));
    json_add_int(live_json_, "drops", static_cast<long>(live_drops_));
    json_add_int(live_json_, "attach_failures",
                 static_cast<long>(live_attach_failures_));
    if (live_last_frame_ms_) {
        json_add_int(live_json_, "last_age_ms", now_ms - live_last_frame_ms_);
    } else {
        live_json_ += ",\"last_age_ms\":null";
    }
    json_add_string(live_json_, "last_error", live_last_error_);
    live_json_ += ",\"samples\":{";
    append_live_series(live_json_, "pressure", live_pressure_, false);
    append_live_series(live_json_, "flow", live_flow_);
    append_live_series(live_json_, "leak", live_leak_);
    append_live_series(live_json_, "inspiratory_pressure",
                       live_inspiratory_pressure_);
    append_live_series(live_json_, "expiratory_pressure",
                       live_expiratory_pressure_);
    append_live_series(live_json_, "spo2", live_spo2_);
    append_live_series(live_json_, "pulse", live_pulse_);
    live_json_ += "}}";

    if (events_->send(live_json_.c_str(), "live", now_ms) !=
        AsyncEventSource::ENQUEUED) {
        sse_enforce_needed_ = true;
    }
    live_last_send_ms_ = now_ms;
    live_state_dirty_ = false;
    reset_live_batch();
}

void WebUI::reset_live_batch() {
    live_pressure_.count = 0;
    live_flow_.count = 0;
    live_leak_.count = 0;
    live_inspiratory_pressure_.count = 0;
    live_expiratory_pressure_.count = 0;
    live_spo2_.count = 0;
    live_pulse_.count = 0;
}

void WebUI::handle_event(const RpcEvent &event) {
    if (!ManagementConsole::event_has_output(event)) return;

    StringPrint capture;
    web_console_.handle_event(capture, event);
    if (!capture.text().length()) return;
    append_console_log(capture.text());
}

void WebUI::append_console_log(const String &text) {
    if (!text.length()) return;
    console_log_ += text;
    while (console_log_.length() > AC_WEB_CONSOLE_LOG_MAX) {
        int newline = console_log_.indexOf('\n',
                                           console_log_.length() -
                                               AC_WEB_CONSOLE_LOG_MAX);
        if (newline < 0) {
            console_log_.remove(0,
                                console_log_.length() -
                                    AC_WEB_CONSOLE_LOG_MAX);
            break;
        }
        console_log_.remove(0, newline + 1);
    }
    console_seq_++;
    snapshots_dirty_ = true;
}

void WebUI::build_console_json(LargeTextBuffer &json) const {
    json = "{";
    json_add_int(json, "seq", console_seq_, false);
    json_add_string(json, "log", console_log_.c_str());
    json += '}';
}

void WebUI::send_cached_settings(AsyncWebServerRequest *request,
                                 int requested_mode) {
    bool mismatch = false;
    bool has_cached = false;
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"valid\":false,\"refresh_queued\":true,"
                      "\"settings\":[]}");
        return;
    }
    if (requested_mode >= 0 && requested_mode != cached_settings_mode_) {
        requested_settings_mode_ = requested_mode;
        cached_settings_refresh_queued_ = true;
        snapshots_dirty_ = true;
        mismatch = true;
    } else {
        has_cached = cached_settings_json_.length() > 0;
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
        settings_placeholder_json(requested_mode, mismatch);
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
        delete command;
        return false;
    }
    WebCommand *queued = command;
    if (xQueueSend(command_queue_, &queued, 0) != pdTRUE) {
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
    const As11DeviceState &as11 = arbiter_->as11_state();
    const WifiManagerStats &wifi_stats = wifi_manager_->stats();
    const MemoryStatus mem = Memory::status();
    const StorageStatus storage = Storage::status();
    const StorageWriterStatus writer = StorageWriter::status();
    const SessionStatus &session = session_manager_->status();
    const SinkRuntimeStatus &sink = sink_manager_->status();

    json = "{";
    json_add_string(json, "version", aircannect_version(), false);
    json_add_string(json, "built", aircannect_build_date());
    json_add_int(json, "uptime", millis() / 1000);
    json_add_int(json, "heap", static_cast<long>(mem.heap_free));
    json_add_int(json, "heap_total", static_cast<long>(mem.heap_total));
    json_add_int(json, "heap_max_alloc",
                 static_cast<long>(mem.heap_max_alloc));
    json_add_bool(json, "psram_available", mem.psram_available);
    json_add_int(json, "psram_free", static_cast<long>(mem.psram_free));
    json_add_int(json, "psram_total", static_cast<long>(mem.psram_total));
    json_add_int(json, "psram_max_alloc",
                 static_cast<long>(mem.psram_max_alloc));
    json_add_bool(json, "storage_configured", storage.configured);
    json_add_bool(json, "storage_mounted", storage.mounted);
    json_add_string(json, "storage_type",
                    Storage::type_name(storage.type));
    json_add_string(json, "storage_state",
                    Storage::state_name(storage.state));
    json_add_string(json, "storage_card", storage.card_type);
    json_add_string(json, "storage_mount", storage.mount_point);
    json_add_string(json, "storage_last_error", storage.last_error);
    json_add_int(json, "storage_width", storage.width);
    json_add_uint64(json, "storage_total", storage.total_bytes);
    json_add_uint64(json, "storage_used", storage.used_bytes);
    json_add_uint64(json, "storage_free", storage.free_bytes);
    json_add_bool(json, "storage_writer_available", writer.available);
    json_add_bool(json, "storage_writer_psram", writer.using_psram);
    json_add_int(json, "storage_writer_q", writer.queued);
    json_add_int(json, "storage_writer_capacity", writer.capacity);
    json_add_int(json, "storage_writer_chunk", writer.chunk_bytes);
    json_add_int(json, "storage_writer_drops", writer.queue_drops);
    json_add_int(json, "storage_writer_unavailable_drops",
                 writer.unavailable_drops);
    json_add_int(json, "storage_writer_errors",
                 writer.open_errors + writer.write_errors);
    json_add_uint64(json, "storage_writer_bytes_written",
                    writer.bytes_written);
    json_add_string(json, "wifi_state", wifi_manager_->state_name());
    json_add_string(json, "wifi_ssid", wifi_manager_->sta_ssid().c_str());
    json_add_string(json, "wifi_ip",
                    wifi_manager_->ip().toString().c_str());
    json_add_string(json, "softap_mode",
                    softap_mode_name(wifi_manager_->softap_mode()));
    json_add_bool(json, "softap_running", wifi_manager_->softap_running());
    char bssid_text[AC_WIFI_BSSID_TEXT_MAX];
    wifi_manager_->bssid(bssid_text, sizeof(bssid_text));
    json_add_int(json, "wifi_rssi", wifi_manager_->rssi());
    json_add_int(json, "wifi_channel", wifi_manager_->channel());
    json_add_string(json, "wifi_bssid", bssid_text);
    json_add_int(json, "wifi_profile", wifi_manager_->active_profile_index());
    json_add_bool(json, "wifi_roam", wifi_manager_->roaming_enabled());
    json_add_bool(json, "wifi_roam_suspended",
                  wifi_manager_->roaming_suspended());
    json_add_int(json, "wifi_attempts", wifi_stats.connect_attempts);
    json_add_int(json, "wifi_failures", wifi_stats.connect_failures);
    json_add_int(json, "wifi_disconnects", wifi_stats.disconnects);
    json_add_int(json, "wifi_roam_scans", wifi_stats.roam_scans);
    json_add_int(json, "wifi_roam_switches", wifi_stats.roam_switches);
    json_add_int(json, "wifi_roam_candidates",
                 wifi_stats.last_roam_candidates);
    json_add_int(json, "wifi_last_reason",
                 wifi_stats.last_disconnect_reason);
    json_add_bool(json, "tcp_started", tcp_bridge_->started());
    json_add_bool(json, "ota_active", ota_manager_->active());
    json_add_bool(json, "ota_ready", ota_manager_->status().http_ready);
    json_add_string(json, "device_name", as11.product_name().c_str());
    json_add_string(json, "serial", as11.serial_number().c_str());
    json_add_string(json, "software_id",
                    as11.software_identifier().c_str());
    json_add_string(json, "therapy",
                    As11DeviceState::therapy_state_name(
                        as11.therapy_state()));
    json_add_string(json, "therapy_pending",
                    As11DeviceState::therapy_target_name(
                        as11.pending_therapy_target()));
    json_add_string(json, "rop", as11.rop().c_str());
    json_add_string(json, "activity_event",
                    as11.last_activity_event().c_str());
    json_add_string(json, "activity_event_time",
                    as11.last_activity_event_report_time().c_str());
    if (as11.last_activity_event_ms()) {
        json_add_int(json, "activity_event_age_ms",
                     millis() - as11.last_activity_event_ms());
    } else {
        json += ",\"activity_event_age_ms\":null";
    }
    json_add_string(json, "profile",
                    as11.active_therapy_profile().c_str());
    String hours = motor_hours(as11.mhr());
    json_add_string(json, "motor_hours", hours.c_str());
    json_add_bool(json, "session_active",
                  session.state == SessionState::Active);
    json_add_int(json, "session_id", session.session_id);
    json_add_int(json, "session_frames", session.frame_count);
    json_add_int(json, "session_drops", session.dropped_frames);
    json_add_int(json, "session_stream_id", session.stream_id);
    json_add_string(json, "session_start_device_time",
                    session.start_device_time);
    json_add_string(json, "session_end_device_time",
                    session.end_device_time);
    json_add_string(json, "session_end_reason", session.end_reason);
    if (session.state == SessionState::Active && session.started_ms) {
        json_add_int(json, "session_age_ms", millis() - session.started_ms);
    } else {
        json += ",\"session_age_ms\":null";
    }
    if (session.last_frame_ms) {
        json_add_int(json, "session_last_frame_age_ms",
                     millis() - session.last_frame_ms);
    } else {
        json += ",\"session_last_frame_age_ms\":null";
    }
    json_add_bool(json, "sink_debug_enabled", sink.debug_enabled);
    json_add_bool(json, "sink_debug_attached", sink.debug_stream_attached);
    json_add_int(json, "sink_frames", sink.frames);
    json_add_int(json, "sink_drops", sink.frame_drops);
    json_add_int(json, "sink_attach_failures", sink.attach_failures);
    json_add_string(json, "sink_last_error", sink.last_error);
    json_add_string(json, "device_datetime",
                    as11.device_datetime().c_str());
    if (as11.clock_valid()) {
        json_add_int(json, "device_datetime_age_ms",
                     millis() - as11.clock_sample_ms());
    } else {
        json += ",\"device_datetime_age_ms\":null";
    }
    if (as11.clock_offset_valid()) {
        json_add_int(json, "device_clock_offset_ms",
                     as11.clock_offset_ms());
    } else {
        json += ",\"device_clock_offset_ms\":null";
    }
    json_add_string(json, "time_sync_policy",
                    "ntp_with_resmed_fallback");
    json_add_bool(json, "resmed_time_sync_enabled",
                  app_config_->data().resmed_time_sync_enabled);
    json_add_string(json, "time_sync_status",
                    time_sync_service_->last_status());
    json_add_bool(json, "ntp_synced", time_sync_service_->ntp_synced());
    json_add_bool(json, "esp_time_valid",
                  time_sync_service_->esp_clock_valid());
    json_add_string(json, "esp_time_source",
                    time_sync_service_->esp_clock_source_name());
    const std::string esp_datetime = time_sync_service_->utc_now_iso();
    json_add_string(json, "esp_datetime", esp_datetime.c_str());
    if (as11.timezone_offset_valid()) {
        json_add_int(json, "timezone_offset_min",
                     as11.timezone_offset_minutes());
    } else {
        json += ",\"timezone_offset_min\":null";
    }
    json += '}';
}

void WebUI::build_stream_json(LargeTextBuffer &json) const {
    const StreamBroker &stream = arbiter_->stream_broker();
    const RpcArbiterStats &stats = arbiter_->stats();

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
    json_add_bool(json, "web_live_attached", live_stream_active());
    json_add_int(json, "web_live_handle", live_stream_handle_);
    json_add_int(json, "web_live_frames", static_cast<long>(live_frames_));
    json_add_int(json, "web_live_drops", static_cast<long>(live_drops_));
    json_add_int(json, "web_live_attach_failures",
                 static_cast<long>(live_attach_failures_));
    json_add_string(json, "web_live_error", live_last_error_);
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
    json_add_bool(json, "http_auth_required", network_auth_required(cfg));
    json_add_string(json, "http_user", cfg.http_user.c_str());
    json_add_bool(json, "http_password_set", cfg.http_password.length() > 0);
    json_add_string(json, "http_password", "");
    json_add_string(json, "auth_whitelist", cfg.auth_whitelist.c_str());
    json_add_bool(json, "telnet_enabled", cfg.telnet_console_enabled);
    json_add_int(json, "telnet_port", cfg.telnet_console_port);
    json_add_bool(json, "ota_auth", cfg.ota_auth_enabled);
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
        json_add_bool(json, "open", profile.open);
        json_add_bool(json, "enabled", profile.enabled);
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
    int mode = requested_mode >= 0 ? requested_mode : active_mode;
    const uint16_t supported_modes = state.supported_mode_mask();
    if (mode >= 0 && supported_modes && !(supported_modes & (1u << mode))) {
        mode = active_mode;
    }

    json = "{";
    json_add_bool(json, "valid", state.valid(), false);
    json_add_bool(json, "refresh_queued", refresh_queued);
    if (mode >= 0) json_add_int(json, "mode", mode);
    else json += ",\"mode\":null";
    json_add_string(json, "mode_name", as11_mode_name(mode));
    if (active_mode >= 0) json_add_int(json, "active_mode", active_mode);
    else json += ",\"active_mode\":null";
    json_add_string(json, "active_mode_name", as11_mode_name(active_mode));
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
        if (!state.setting_visible(i, mode)) continue;
        if (!as11_setting_readable_via_rpc(def)) continue;

        std::string value = state.value(i, mode);
        const bool available = !value.empty() || state.pending(i);
        const bool inferred =
            strcmp(def.name, "TherapyMode") == 0 &&
            mode >= 0 &&
            mode != active_mode;

        if (emitted++) json += ',';
        json += "{";
        json_add_string(json, "name", def.name, false);
        json_add_string(json, "label", def.label);
        json_add_string(json, "group", def.group);
        json_add_string(json, "kind", setting_kind_name(def.kind));
        json_add_string(json, "value", value.c_str());
        std::string display = as11_setting_display_value(def, value);
        json_add_string(json, "display", display.c_str());
        json_add_bool(json, "available", available);
        json_add_bool(json, "inferred", inferred);
        json_add_bool(json, "writable",
                      as11_setting_writable_via_rpc(def, mode));
        json_add_bool(json, "pending", state.pending(i));
        json_add_string(json, "pending_value",
                        state.pending_value(i).c_str());
        std::string pending_display =
            as11_setting_display_value(def, state.pending_value(i));
        json_add_string(json, "pending_display", pending_display.c_str());
        if (state.pending(i)) {
            json_add_int(json, "pending_age_ms",
                         millis() - state.pending_since_ms(i));
        } else {
            json += ",\"pending_age_ms\":null";
        }
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
                if (strcmp(def.name, "TherapyMode") == 0) {
                    const uint16_t modes = state.supported_mode_mask();
                    if (!modes || !(modes & (1u << opt_index))) continue;
                }
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
    const bool dirty = snapshots_dirty_ || !snapshots_ready_;

    if (!force && !dirty &&
        static_cast<int32_t>(now - last_snapshot_ms_) <
            static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS)) {
        xSemaphoreGive(cache_mutex_);
        return;
    }

    const int requested_mode = requested_settings_mode_;
    const bool refresh_queued = cached_settings_refresh_queued_;
    int published_settings_mode = requested_mode;
    if (published_settings_mode < 0) {
        published_settings_mode = arbiter_->as11_settings().mode_index();
        if (published_settings_mode < 0) {
            published_settings_mode = as11_mode_index_from_value(
                arbiter_->as11_state().active_therapy_profile());
        }
    }

    build_status_json(cached_status_json_);
    build_stream_json(cached_stream_json_);
    build_console_json(cached_console_json_);
    build_config_json(cached_config_json_);
    build_wifi_json(cached_wifi_json_);
    build_ota_json(cached_ota_json_, ota_manager_->status());
    build_resmed_ota_json(cached_resmed_ota_json_, *resmed_ota_manager_);
    build_settings_json(cached_settings_json_, requested_mode, refresh_queued);
    cached_settings_mode_ = published_settings_mode;
    cached_settings_refresh_queued_ = false;
    cached_http_auth_required_ = network_auth_required(app_config_->data());
    cached_http_user_ = app_config_->data().http_user;
    cached_http_password_ = app_config_->data().http_password;
    cached_auth_whitelist_ = app_config_->data().auth_whitelist;
    snapshots_ready_ = true;
    snapshots_dirty_ = false;
    last_snapshot_ms_ = now;
    xSemaphoreGive(cache_mutex_);
}

void WebUI::execute_command(WebCommand &command) {
    switch (command.kind) {
        case WebCommandConsoleLine:
            execute_console_line(command.text);
            break;
        case WebCommandConsoleClear:
            console_log_ = "";
            console_seq_++;
            snapshots_dirty_ = true;
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
            snapshots_dirty_ = true;
            break;
        case WebCommandSettingsUpdate:
            execute_settings_update(command.body);
            break;
        case WebCommandTherapyAction:
            execute_therapy_action(command.text);
            break;
        case WebCommandResmedOtaInit:
        case WebCommandResmedOtaBlock:
        case WebCommandResmedOtaCheck:
        case WebCommandResmedOtaApply:
        case WebCommandResmedOtaAbort:
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
    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) return;
    size_t saved = 0;
    bool reconnect = false;
    String s;
    app_config_->begin_update();
    if (json_get_string(doc, "hostname", s) &&
        app_config_->set_hostname(s)) {
        wifi_manager_->set_hostname(app_config_->data().hostname);
        ota_manager_->mark_config_dirty();
        saved++;
    }
    if (json_get_string(doc, "wifi_country", s) &&
        app_config_->set_wifi_country(s)) {
        wifi_manager_->set_country_code(app_config_->data().wifi_country);
        reconnect = true;
        saved++;
    }
    if (json_get_string(doc, "timezone", s) &&
        app_config_->set_timezone(s)) {
        saved++;
    }
    if (doc["resmed_time_sync_enabled"].is<bool>() &&
        app_config_->set_resmed_time_sync(
            doc["resmed_time_sync_enabled"].as<bool>())) {
        saved++;
    }
    if (json_get_string(doc, "softap_mode", s)) {
        SoftApMode softap_mode;
        if (parse_softap_mode(s, softap_mode)) {
            const WifiModeState wifi_mode = wifi_manager_->mode_state();
            const bool was_softap = wifi_mode == WifiModeState::SoftAp;
            if (app_config_->set_softap_mode(softap_mode)) {
                wifi_manager_->set_softap_mode(
                    app_config_->data().softap_mode);
                wifi_manager_->apply_softap_mode();
                reconnect = reconnect ||
                            (was_softap &&
                             app_config_->data().softap_mode ==
                                 SoftApMode::Auto &&
                             wifi_manager_->has_sta_config());
                saved++;
            }
        }
    }
    const bool http_auth_present = doc["http_auth_required"].is<bool>();
    const bool disable_http_auth =
        http_auth_present && !doc["http_auth_required"].as<bool>();
    const bool http_user_present = doc["http_user"].is<const char *>();
    const bool http_password_present =
        doc["http_password"].is<const char *>();
    if (disable_http_auth || http_user_present || http_password_present) {
        String user = app_config_->data().http_user;
        String password = app_config_->data().http_password;
        if (disable_http_auth) {
            user = "";
            password = "";
        } else if (http_user_present) {
            user = doc["http_user"].as<const char *>();
        }
        if (!disable_http_auth && http_password_present) {
            password = doc["http_password"].as<const char *>();
        }
        if (app_config_->set_http_auth(user, password)) saved++;
    }
    if (json_get_string(doc, "auth_whitelist", s) &&
        app_config_->set_auth_whitelist(s)) {
        saved++;
    }
    if (doc["ota_auth"].is<bool>() &&
        app_config_->set_ota_auth_enabled(doc["ota_auth"].as<bool>())) {
        ota_manager_->mark_config_dirty();
        saved++;
    }
    if (json_get_string(doc, "ota_password", s) && s.length() &&
        app_config_->set_ota_password(s)) {
        ota_manager_->mark_config_dirty();
        saved++;
    }
    if (doc["tcp_enabled"].is<bool>() || doc["tcp_port"].is<int>()) {
        bool enabled = app_config_->data().tcp_bridge_enabled;
        uint16_t port = app_config_->data().tcp_bridge_port;
        if (doc["tcp_enabled"].is<bool>()) enabled = doc["tcp_enabled"].as<bool>();
        if (doc["tcp_port"].is<int>()) {
            int parsed = doc["tcp_port"].as<int>();
            if (parsed > 0 && parsed <= 65535) {
                port = static_cast<uint16_t>(parsed);
            }
        }
        if (app_config_->set_tcp_bridge(enabled, port)) saved++;
    }
    if (doc["telnet_enabled"].is<bool>() || doc["telnet_port"].is<int>()) {
        bool enabled = app_config_->data().telnet_console_enabled;
        uint16_t port = app_config_->data().telnet_console_port;
        if (doc["telnet_enabled"].is<bool>()) {
            enabled = doc["telnet_enabled"].as<bool>();
        }
        if (doc["telnet_port"].is<int>()) {
            int parsed = doc["telnet_port"].as<int>();
            if (parsed > 0 && parsed <= 65535) {
                port = static_cast<uint16_t>(parsed);
            }
        }
        if (app_config_->set_telnet_console(enabled, port)) saved++;
    }
    const bool config_committed = app_config_->commit_update();
    if (!config_committed) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] failed to persist one or more config values\n");
    }
    if (reconnect) wifi_manager_->reconnect();
    if (saved) snapshots_dirty_ = true;
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
    if (changed) snapshots_dirty_ = true;
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
    snapshots_dirty_ = true;
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
        snapshots_dirty_ = true;
    }
}

void WebUI::execute_therapy_action(const std::string &action) {
    const char *method = nullptr;
    if (action == "start") method = "EnterTherapy";
    if (action == "stop" || action == "standby") method = "EnterStandby";
    if (!method) return;
    arbiter_->send_request(method, "", RpcSource::HttpApi);
    snapshots_dirty_ = true;
}

void WebUI::execute_resmed_ota_command(const WebCommand &command) {
    if (command.kind == WebCommandResmedOtaCheck) {
        resmed_ota_manager_->request_check();
        snapshots_dirty_ = true;
        return;
    }
    if (command.kind == WebCommandResmedOtaAbort) {
        resmed_ota_manager_->abort("aborted");
        snapshots_dirty_ = true;
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
    snapshots_dirty_ = true;
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
        send_cached(request, cached_console_json_);
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
            if (index == 0 && !ota_manager_->begin_http_upload(filename)) {
                return;
            }
            ota_manager_->write_http_upload(data, len);
        });

    server_->on("/api/resmed-ota", HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_cached(request, cached_resmed_ota_json_);
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

    server_->on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest *request) {
        const int mode = request_mode_arg(request);
        if (request->hasArg("refresh")) {
            if (cache_mutex_ &&
                xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                cached_settings_refresh_queued_ = true;
                snapshots_dirty_ = true;
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
