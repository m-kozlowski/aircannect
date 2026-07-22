#include "web_ui.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <string.h>
#include <utility>

#include "auth_utils.h"
#include "board.h"
#include "debug_log.h"
#include "http_route_module.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "memory_manager.h"
#include "sink_manager.h"
#include "status_http_controller.h"
#include "string_util.h"
#include "string_print.h"
#include "web_ui_html.h"

namespace aircannect {

namespace {

static constexpr uint32_t WEB_LIVE_VIEW_LEASE_MS = 12000;

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

uint32_t fnv1a32_string(const String &text) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < text.length(); ++i) {
        hash ^= static_cast<uint8_t>(text[i]);
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

bool json_get_string(JsonDocument &doc, const char *key, String &out) {
    if (!doc[key].is<const char *>()) return false;
    out = doc[key].as<const char *>();
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

bool WebUI::begin(StatusHttpController &status,
                  StreamBroker &stream,
                  SinkManager &sink_manager,
                  ConsoleContext &console_ctx,
                  const AppConfigData &config,
                  HttpRouteModule *const *route_modules,
                  size_t route_module_count,
                  uint16_t port) {
    if (started_) return true;
    stop();
    status_ = &status;
    stream_ = &stream;
    sink_manager_ = &sink_manager;
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

void WebUI::reserve_cached_json() {
    cached_status_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    cached_stream_json_.reserve(AC_WEB_STREAM_JSON_RESERVE);
    next_status_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    next_stream_json_.reserve(AC_WEB_STREAM_JSON_RESERVE);
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
    observed_status_revision_ = 0;
    last_snapshot_ms_ = 0;
    last_sse_push_ms_ = 0;
    sse_push_requested_ = false;
    status_ = nullptr;
    stream_ = nullptr;
    started_ = false;
}

void WebUI::poll(PollCheckpoint checkpoint) {
    if (!started_) return;
    publish_pending_auth_config();

    if (status_ && observed_status_revision_ != status_->revision()) {
        mark_snapshots_dirty(SNAPSHOT_STATUS);
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
    publish_snapshots(false, checkpoint);
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
    next_stream_json_.clear();
    uint16_t completed_mask = 0;

    if (rebuild_mask & SNAPSHOT_STATUS) {
        uint32_t revision = observed_status_revision_;
        if (status_ && status_->copy_snapshot(next_status_json_, revision)) {
            observed_status_revision_ = revision;
            completed_mask |= SNAPSHOT_STATUS;
        }
        if (checkpoint) checkpoint("web_ui.snapshots.status_copy");
    }
    if (rebuild_mask & SNAPSHOT_STREAM) {
        build_stream_json(next_stream_json_);
        if (!next_stream_json_.overflowed()) {
            completed_mask |= SNAPSHOT_STREAM;
        }
        if (checkpoint) checkpoint("web_ui.snapshots.stream");
    }
    if (!completed_mask || !cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        return;
    }

    if (completed_mask & SNAPSHOT_STATUS) {
        cached_status_json_.swap(next_status_json_);
    }
    if (completed_mask & SNAPSHOT_STREAM) {
        cached_stream_json_.swap(next_stream_json_);
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
    if (!console_ctx_) return;

    StringPrint capture(AC_WEB_CONSOLE_COMMAND_OUTPUT_MAX);
    web_console_.execute_line(command, capture, *console_ctx_);
    String entry = "> ";
    entry += command;
    entry += "\n";
    entry += capture.text();
    append_console_log(entry);
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
