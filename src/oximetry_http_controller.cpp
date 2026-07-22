#include "oximetry_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <string.h>
#include <utility>

#include "board.h"
#include "config_service.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "json_util.h"

namespace aircannect {
namespace {

const char *sensor_state_name(OximetrySensorState state) {
    switch (state) {
        case OximetrySensorState::Off: return "off";
        case OximetrySensorState::Idle: return "idle";
        case OximetrySensorState::Scanning: return "scanning";
        case OximetrySensorState::Connecting: return "connecting";
        case OximetrySensorState::Connected: return "connected";
        case OximetrySensorState::Streaming: return "streaming";
    }
    return "unknown";
}

void append_sensor(LargeTextBuffer &json,
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

bool build_sensor_json(LargeTextBuffer &json,
                       const OximetryManager &oximetry) {
    const OximetryRuntimeStatus runtime = oximetry.runtime_status();
    const OximetrySensorStatus sensor = oximetry.sensor_status();
    OximetrySensorDevice scan[AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
    OximetrySensorDevice known[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    const size_t scan_count = oximetry.sensor_scan_results(
        scan, AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS);
    const size_t known_count = oximetry.known_sensors(
        known, AC_OXIMETRY_SENSOR_MAX_KNOWN);

    json = "{";
    json_add_bool(json, "enabled", runtime.enabled, false);
    json_add_bool(json, "ble_available", runtime.ble_available);
    json_add_string(json, "sensor_state",
                    sensor_state_name(sensor.sensor_state));
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
    for (size_t i = 0; i < scan_count; ++i) {
        if (i) json += ',';
        append_sensor(json, scan[i], i, true);
    }
    json += "],\"sensor_known\":[";
    for (size_t i = 0; i < known_count; ++i) {
        if (i) json += ',';
        append_sensor(json, known[i], i, false);
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

void copy_json_text(JsonVariantConst value, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!value.is<const char *>()) return;

    strlcpy(out, value.as<const char *>(), out_size);
}

}  // namespace

bool OximetryHttpController::begin(OximetryManager &oximetry,
                                   ConfigService &config) {
    oximetry_ = &oximetry;
    config_ = &config;
    if (!commands_.begin()) return false;

    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!cache_mutex_) return false;

    snapshot_json_.reserve(AC_WEB_OXIMETRY_SENSORS_JSON_RESERVE);
    return publish_snapshot();
}

void OximetryHttpController::register_routes(AsyncWebServer &server) {
    server.on(
        AsyncURIMatcher::exact("/api/oximetry"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { send_action(request); },
        nullptr, http_request_body_handler);

    server.on(AsyncURIMatcher::exact("/api/oximetry/sensors"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_snapshot(request);
    });
}

void OximetryHttpController::poll() {
    if (!oximetry_ || !config_) return;

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

bool OximetryHttpController::enqueue(Command &&command) {
    const bool queued = commands_.push(std::move(command));
    if (!queued) {
        Log::logf(CAT_OXI, LOG_WARN,
                  "HTTP oximetry command queue full\n");
    }
    return queued;
}

void OximetryHttpController::execute(Command &command) {
    switch (command.kind) {
        case CommandKind::Enable:
            (void)config_->set_value("oxi_en", "1", false);
            break;
        case CommandKind::Disable:
            (void)config_->set_value("oxi_en", "0", false);
            break;
        case CommandKind::PairStart:
            (void)config_->set_value("oxi_en", "1", false);
            oximetry_->request_pairing(true);
            break;
        case CommandKind::PairStop:
            oximetry_->request_pairing(false);
            break;
        case CommandKind::ForgetBonds:
            oximetry_->forget_bonds();
            break;
        case CommandKind::AdvertiseStart:
            oximetry_->request_advertising(true);
            break;
        case CommandKind::AdvertiseStop:
            oximetry_->request_advertising(false);
            break;
        case CommandKind::SensorScan:
            oximetry_->request_sensor_scan();
            break;
        case CommandKind::SensorDisconnect:
            oximetry_->request_sensor_disconnect();
            break;
        case CommandKind::SensorConnect: {
            const bool accepted = command.has_device
                ? oximetry_->request_sensor_connect_device(command.device)
                : oximetry_->request_sensor_connect(command.target.c_str());
            if (!accepted) {
                Log::logf(CAT_OXI, LOG_WARN,
                          "sensor connect command rejected target=\"%s\" "
                          "addr=\"%s\"\n",
                          command.target.c_str(), command.device.addr);
            }
            break;
        }
        case CommandKind::SensorForget:
            oximetry_->forget_sensor(command.target.c_str());
            break;
        case CommandKind::SensorAutoconnect:
            oximetry_->set_sensor_autoconnect(command.target.c_str(),
                                              command.enabled);
            break;
    }

    snapshot_dirty_ = true;
}

bool OximetryHttpController::publish_snapshot() {
    LargeTextBuffer next;
    next.reserve(AC_WEB_OXIMETRY_SENSORS_JSON_RESERVE);
    if (!build_sensor_json(next, *oximetry_)) return false;

    if (xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return false;
    snapshot_json_.swap(next);
    last_snapshot_ms_ = millis();
    snapshot_dirty_ = false;
    xSemaphoreGive(cache_mutex_);
    return true;
}

void OximetryHttpController::send_snapshot(
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

void OximetryHttpController::send_action(
    AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!http_parse_json_body(request, doc, body)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    const char *action = doc["action"] | "";
    Command command;
    if (strcmp(action, "enable") == 0) {
        command.kind = CommandKind::Enable;
    } else if (strcmp(action, "disable") == 0) {
        command.kind = CommandKind::Disable;
    } else if (strcmp(action, "pair") == 0) {
        command.kind = CommandKind::PairStart;
    } else if (strcmp(action, "pair_stop") == 0) {
        command.kind = CommandKind::PairStop;
    } else if (strcmp(action, "forget") == 0) {
        command.kind = CommandKind::ForgetBonds;
    } else if (strcmp(action, "advertise_start") == 0) {
        command.kind = CommandKind::AdvertiseStart;
    } else if (strcmp(action, "advertise_stop") == 0) {
        command.kind = CommandKind::AdvertiseStop;
    } else if (strcmp(action, "sensor_scan") == 0) {
        command.kind = CommandKind::SensorScan;
    } else if (strcmp(action, "sensor_disconnect") == 0) {
        command.kind = CommandKind::SensorDisconnect;
    } else if (strcmp(action, "sensor_connect") == 0) {
        command.kind = CommandKind::SensorConnect;
        command.target = doc["target"] | "";
        copy_json_text(doc["addr"], command.device.addr,
                       sizeof(command.device.addr));
        command.has_device = command.device.addr[0] != 0;
        if (command.has_device) {
            command.device.addr_type = doc["addr_type"] | 1;
            copy_json_text(doc["name"], command.device.name,
                           sizeof(command.device.name));
            command.device.rssi = doc["rssi"] | 0;
        } else if (command.target.empty()) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"missing_target\"}");
            return;
        }
    } else if (strcmp(action, "sensor_forget") == 0) {
        command.kind = CommandKind::SensorForget;
        command.target = doc["addr"] | "";
    } else if (strcmp(action, "sensor_autoconnect") == 0 &&
               doc["enabled"].is<bool>()) {
        command.kind = CommandKind::SensorAutoconnect;
        command.target = doc["addr"] | "";
        command.enabled = doc["enabled"].as<bool>();
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
