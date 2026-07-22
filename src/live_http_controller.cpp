#include "live_http_controller.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "json_util.h"
#include "sink_manager.h"
#include "stream_broker.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t WEB_LIVE_VIEW_LEASE_MS = 12000;

uint32_t fnv1a32_string(const String &text) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < text.length(); ++i) {
        hash ^= static_cast<uint8_t>(text[i]);
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
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

bool build_stream_json(LargeTextBuffer &json,
                       const StreamBroker &stream,
                       const SinkManager &sink) {
    const LiveChartRuntimeStatus &live = sink.live_chart_status();

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
        const StreamConsumerHandle handle =
            static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;
        if (!first_consumer) json += ',';
        first_consumer = false;

        json += '{';
        json_add_bool(json, "active", true, false);
        json_add_int(json, "source",
                     static_cast<unsigned>(stream.consumer_source(handle)));
        json_add_int(json, "queued", stream.consumer_queue_count(handle));
        json_add_int(json, "drops", stream.consumer_queue_drops(handle));
        json += '}';
    }
    json += "]}";
    return !json.overflowed();
}

}  // namespace

bool LiveHttpController::begin(StreamBroker &stream, SinkManager &sink) {
    stream_ = &stream;
    sink_ = &sink;

    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!lease_mutex_) {
        lease_mutex_ = xSemaphoreCreateMutexStatic(&lease_mutex_storage_);
    }
    if (!cache_mutex_ || !lease_mutex_) return false;

    stream_json_.reserve(AC_WEB_STREAM_JSON_RESERVE);
    stream_build_json_.reserve(AC_WEB_STREAM_JSON_RESERVE);
    live_json_.reserve(4096);
    return publish_stream_snapshot();
}

void LiveHttpController::stop() {
    if (sink_) {
        sink_->set_live_chart_enabled(false);
        sink_->clear_live_chart_batch();
    }

    if (lease_mutex_ &&
        xSemaphoreTake(lease_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (LiveViewLease &lease : leases_) lease = {};
        xSemaphoreGive(lease_mutex_);
    }

    last_live_send_ms_ = 0;
    live_json_.clear();
}

void LiveHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/stream"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_stream_snapshot(request);
    });

    server.on(AsyncURIMatcher::exact("/api/live/view"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        send_live_view_state(request);
    });
}

void LiveHttpController::poll(size_t healthy_sse_clients,
                              uint32_t now_ms) {
    if (!stream_ || !sink_) return;

    const bool live_needed =
        healthy_sse_clients > 0 && live_view_requested(now_ms);
    sink_->set_live_chart_enabled(live_needed);
    if (!live_needed) {
        sink_->clear_live_chart_batch();
        last_live_send_ms_ = 0;
    } else {
        publish_live_payload(now_ms);
    }

    const bool stream_snapshot_due =
        static_cast<int32_t>(now_ms - last_stream_snapshot_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    if (stream_snapshot_due) (void)publish_stream_snapshot();
}

bool LiveHttpController::stream_payload(const char *&data,
                                        size_t &length) const {
    if (!stream_json_.length()) return false;
    data = stream_json_.c_str();
    length = stream_json_.length();
    return true;
}

bool LiveHttpController::live_payload(const char *&data,
                                      size_t &length,
                                      uint32_t &generation) const {
    if (!live_generation_ || !live_json_.length()) return false;
    data = live_json_.c_str();
    length = live_json_.length();
    generation = live_generation_;
    return true;
}

LiveHttpMemoryStatus LiveHttpController::memory_status() const {
    LiveHttpMemoryStatus out;
    out.stream_length = stream_json_.length();
    out.stream_capacity = stream_json_.capacity();
    out.live_length = live_json_.length();
    out.live_capacity = live_json_.capacity();
    return out;
}

bool LiveHttpController::live_view_requested(uint32_t now_ms) {
    if (!lease_mutex_ ||
        xSemaphoreTake(lease_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return false;
    }

    bool requested = false;
    for (LiveViewLease &lease : leases_) {
        if (!lease.client_hash) continue;
        if (static_cast<int32_t>(now_ms - lease.expires_ms) >= 0) {
            lease = {};
            continue;
        }
        requested = true;
    }
    xSemaphoreGive(lease_mutex_);
    return requested;
}

bool LiveHttpController::publish_stream_snapshot() {
    stream_build_json_.clear();
    if (!build_stream_json(stream_build_json_, *stream_, *sink_)) {
        return false;
    }

    if (xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return false;
    stream_json_.swap(stream_build_json_);
    last_stream_snapshot_ms_ = millis();
    xSemaphoreGive(cache_mutex_);
    return true;
}

void LiveHttpController::publish_live_payload(uint32_t now_ms) {
    const LiveChartRuntimeStatus &live = sink_->live_chart_status();
    if (!live.desired) return;

    const bool has_samples =
        live.pressure.count || live.flow.count || live.leak.count ||
        live.inspiratory_pressure.count ||
        live.expiratory_pressure.count || live.spo2.count ||
        live.pulse.count;
    const bool interval_due =
        static_cast<int32_t>(now_ms - last_live_send_ms_) >=
        static_cast<int32_t>(AC_WEB_LIVE_PUSH_INTERVAL_MS);
    const bool heartbeat_due =
        static_cast<int32_t>(now_ms - last_live_send_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    if (has_samples && !interval_due) return;
    if (!has_samples && !live.state_dirty && !heartbeat_due) return;

    uint32_t next_generation = live_generation_ + 1;
    if (next_generation == 0) next_generation = 1;
    live_json_ = "{";
    json_add_int(live_json_, "seq",
                 static_cast<long>(next_generation), false);
    json_add_bool(live_json_, "active", live.desired);
    json_add_bool(live_json_, "attached", live.attached);
    json_add_int(live_json_, "frames", static_cast<long>(live.frames));
    json_add_int(live_json_, "drops", static_cast<long>(live.drops));
    json_add_int(live_json_, "attach_failures",
                 static_cast<long>(live.attach_failures));
    if (live.last_frame_ms) {
        json_add_int(live_json_, "last_age_ms",
                     now_ms - live.last_frame_ms);
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
    if (live_json_.overflowed()) return;

    live_generation_ = next_generation;
    last_live_send_ms_ = now_ms;
    sink_->mark_live_chart_sent();
}

void LiveHttpController::send_stream_snapshot(
    AsyncWebServerRequest *request) const {
    if (xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache_busy\"}");
        return;
    }

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (response) {
        response->write(
            reinterpret_cast<const uint8_t *>(stream_json_.c_str()),
            stream_json_.length());
    }
    xSemaphoreGive(cache_mutex_);

    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    request->send(response);
}

void LiveHttpController::send_live_view_state(
    AsyncWebServerRequest *request) {
    bool active = false;
    if (request->hasArg("active")) {
        parse_bool_yesno(request->arg("active"), active);
    } else if (request->hasArg("enabled")) {
        parse_bool_yesno(request->arg("enabled"), active);
    }
    const uint32_t client_hash =
        request->hasArg("id") ? fnv1a32_string(request->arg("id")) : 1u;
    const uint32_t now_ms = millis();

    if (!lease_mutex_ ||
        xSemaphoreTake(lease_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
        request->send(200, "application/json",
                      active ? "{\"ok\":true,\"active\":true}"
                             : "{\"ok\":true,\"active\":false}");
        return;
    }

    LiveViewLease *empty = nullptr;
    LiveViewLease *oldest = nullptr;
    for (LiveViewLease &lease : leases_) {
        if (lease.client_hash == client_hash) {
            if (active) {
                lease.expires_ms = now_ms + WEB_LIVE_VIEW_LEASE_MS;
            } else {
                lease = {};
            }
            xSemaphoreGive(lease_mutex_);
            request->send(200, "application/json",
                          active ? "{\"ok\":true,\"active\":true}"
                                 : "{\"ok\":true,\"active\":false}");
            return;
        }
        if (!lease.client_hash ||
            static_cast<int32_t>(now_ms - lease.expires_ms) >= 0) {
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
    xSemaphoreGive(lease_mutex_);

    request->send(200, "application/json",
                  active ? "{\"ok\":true,\"active\":true}"
                         : "{\"ok\":true,\"active\":false}");
}

}  // namespace aircannect
