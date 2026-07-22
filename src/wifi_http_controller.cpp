#include "wifi_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <utility>

#include "board.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "wifi_manager.h"

namespace aircannect {
namespace {

bool build_wifi_json(LargeTextBuffer &json, const WifiManager &wifi) {
    json = "{";
    json_add_string(json, "state", wifi.state_name(), false);
    json_add_string(json, "ssid", wifi.sta_ssid().c_str());
    json_add_int(json, "active", wifi.active_profile_index());
    json_add_string(json, "ip", wifi.ip().toString().c_str());

    char bssid[AC_WIFI_BSSID_TEXT_MAX];
    wifi.bssid(bssid, sizeof(bssid));
    json_add_string(json, "bssid", bssid);
    json_add_int(json, "rssi", wifi.rssi());
    json_add_int(json, "channel", wifi.channel());
    json_add_bool(json, "roam", wifi.roaming_enabled());

    json += ",\"profiles\":[";
    for (size_t i = 0; i < wifi.profile_count(); ++i) {
        const WifiProfile &profile = wifi.profile(i);
        if (i) json += ',';
        json += '{';
        json_add_string(json, "ssid", profile.ssid.c_str(), false);
        json_add_bool(json, "open", profile.password.length() == 0);
        json += '}';
    }
    json += "]}";
    return !json.overflowed();
}

bool prepare_json_response(AsyncWebServerRequest *request,
                           const LargeTextBuffer &json,
                           AsyncResponseStream *&response) {
    response = request->beginResponseStream("application/json");
    if (!response) return false;

    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    return true;
}

}  // namespace

bool WifiHttpController::begin(WifiManager &wifi) {
    wifi_ = &wifi;
    if (!commands_.begin()) return false;

    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!cache_mutex_) return false;

    snapshot_json_.reserve(AC_WEB_WIFI_JSON_RESERVE);
    return publish_snapshot();
}

void WifiHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/wifi"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_snapshot(request);
    });

    server.on(
        AsyncURIMatcher::exact("/api/wifi"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { send_update(request); },
        nullptr, http_request_body_handler);
}

void WifiHttpController::poll() {
    if (!wifi_) return;

    for (size_t i = 0; i < CommandsPerPoll; ++i) {
        Command command;
        if (!commands_.pop(command)) break;
        execute(command);
    }

    const uint32_t now_ms = millis();
    if (snapshot_dirty_ ||
        static_cast<int32_t>(now_ms - last_snapshot_ms_) >=
            static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS)) {
        (void)publish_snapshot();
    }
}

bool WifiHttpController::enqueue(Command &&command) {
    const bool queued = commands_.push(std::move(command));
    if (!queued) {
        Log::logf(CAT_WIFI, LOG_WARN, "HTTP Wi-Fi command queue full\n");
    }
    return queued;
}

void WifiHttpController::execute(Command &command) {
    bool changed = false;
    const String ssid(command.ssid.c_str());
    const String password(command.password.c_str());

    switch (command.kind) {
        case CommandKind::Set:
            changed = wifi_->configure_sta(ssid, password);
            break;
        case CommandKind::Add:
            changed = wifi_->add_profile(ssid, password, password.isEmpty());
            break;
        case CommandKind::Remove:
            changed = wifi_->remove_profile(command.index);
            break;
        case CommandKind::Clear:
            wifi_->clear_sta_config();
            changed = true;
            break;
        case CommandKind::Reconnect:
            changed = wifi_->reconnect();
            break;
    }

    if (changed) snapshot_dirty_ = true;
}

bool WifiHttpController::publish_snapshot() {
    LargeTextBuffer next;
    next.reserve(AC_WEB_WIFI_JSON_RESERVE);
    if (!build_wifi_json(next, *wifi_)) return false;

    if (xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return false;
    snapshot_json_.swap(next);
    last_snapshot_ms_ = millis();
    snapshot_dirty_ = false;
    xSemaphoreGive(cache_mutex_);
    return true;
}

void WifiHttpController::send_snapshot(
    AsyncWebServerRequest *request) const {
    if (xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache_busy\"}");
        return;
    }

    AsyncResponseStream *response = nullptr;
    const bool prepared =
        prepare_json_response(request, snapshot_json_, response);
    xSemaphoreGive(cache_mutex_);
    if (!prepared) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    request->send(response);
}

void WifiHttpController::send_update(AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!http_parse_json_body(request, doc, body)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    const char *action = doc["action"] | "";
    Command command;
    if (strcmp(action, "set") == 0 || strcmp(action, "add") == 0) {
        if (!doc["ssid"].is<const char *>()) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"missing_ssid\"}");
            return;
        }
        command.kind = strcmp(action, "set") == 0
                           ? CommandKind::Set
                           : CommandKind::Add;
        command.ssid = doc["ssid"].as<const char *>();
        command.password = doc["pass"] | "";
    } else if (strcmp(action, "remove") == 0 && doc["index"].is<int>()) {
        const int index = doc["index"].as<int>();
        if (index < 0) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"bad_index\"}");
            return;
        }
        command.kind = CommandKind::Remove;
        command.index = static_cast<size_t>(index);
    } else if (strcmp(action, "clear") == 0) {
        command.kind = CommandKind::Clear;
    } else if (strcmp(action, "reconnect") == 0) {
        command.kind = CommandKind::Reconnect;
    } else {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"unknown_action\"}");
        return;
    }

    const bool queued = enqueue(std::move(command));
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

}  // namespace aircannect
