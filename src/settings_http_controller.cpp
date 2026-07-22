#include "settings_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <string.h>
#include <utility>

#include "as11_device_service.h"
#include "as11_settings.h"
#include "as11_settings_manager.h"
#include "board.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "rpc_request_port.h"

namespace aircannect {
namespace {

int request_profile_mode(AsyncWebServerRequest *request) {
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

int active_settings_mode(const As11DeviceState &device,
                         const As11SettingsState &settings) {
    int mode = settings.mode_index();
    if (mode < 0) {
        mode = as11_mode_index_from_value(device.active_therapy_profile());
    }
    return mode;
}

const char *setting_kind_name(As11SettingKind kind) {
    switch (kind) {
        case As11SettingKind::Number: return "number";
        case As11SettingKind::Enum: return "enum";
        case As11SettingKind::Bool: return "bool";
        case As11SettingKind::Text: return "text";
    }
    return "text";
}

String settings_placeholder_json(bool refresh_queued,
                                 bool snapshot_pending) {
    String json = "{";
    json_add_bool(json, "valid", false, false);
    json_add_bool(json, "refresh_queued", refresh_queued);
    json_add_bool(json, "snapshot_pending", snapshot_pending);
    json_add_int(json, "pending_count", 0);
    json_add_string(json, "last_write_status", "");
    json += ",\"last_write_age_ms\":null";
    json += ",\"age_ms\":null";
    json += ",\"settings\":[]}";
    return json;
}

void build_settings_json(LargeTextBuffer &json,
                         const As11DeviceState &device,
                         const As11SettingsState &state,
                         int requested_mode,
                         bool refresh_queued) {
    const int active_mode = active_settings_mode(device, state);
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
        if (!state.setting_visible(i, profile_mode) ||
            !as11_setting_readable_via_rpc(def)) {
            continue;
        }

        const bool therapy_mode = strcmp(def.key, "MOP") == 0;
        const std::string value =
            state.value(i, therapy_mode ? active_mode : profile_mode);
        const bool pending = state.pending(i);
        const bool available = !value.empty() || pending;
        if (!available) continue;

        if (emitted++) json += ',';
        json += '{';
        json_add_string(json, "key", def.key, false);
        json_add_string(json, "value", value.c_str());
        if (!as11_setting_writable_via_rpc(def)) {
            json_add_bool(json, "writable", false);
        }
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

void build_catalog_json(LargeTextBuffer &json) {
    json = "{\"settings\":[";
    size_t emitted = 0;
    for (size_t i = 0; i < as11_setting_count(); ++i) {
        const As11SettingDef &def = as11_setting(i);
        if (!def.mode_mask || !as11_setting_readable_via_rpc(def)) continue;

        if (emitted++) json += ',';
        json += '{';
        json_add_string(json, "key", def.key, false);
        json_add_string(json, "label", def.label);
        const std::string rpc_name = as11_setting_rpc_long_name(def);
        json_add_string(json, "rpc_name", rpc_name.c_str());
        json_add_string(json, "group", def.group);
        json_add_string(json, "category", def.category);
        json_add_string(json, "kind", setting_kind_name(def.kind));
        json_add_int(json, "modes", def.mode_mask);
        json_add_float(json, "min", def.min_value);
        json_add_float(json, "max", def.max_value);
        json_add_float(json, "step", def.step);
        json_add_int(json, "scale_div", def.scale_div);
        json_add_int(json, "decimals", def.decimals);
        if (def.options && def.option_count) {
            json += ",\"options\":[";
            for (uint8_t option = 0; option < def.option_count; ++option) {
                if (option) json += ',';
                json += '{';
                json_add_int(json, "value", option, false);
                json_add_string(json, "label", def.options[option]);
                json += '}';
            }
            json += ']';
        }
        json += '}';
    }

    json += "],\"composites\":[";
    for (size_t i = 0; i < as11_setting_composite_count(); ++i) {
        const As11SettingCompositeDef &def = as11_setting_composite(i);
        if (i) json += ',';
        json += '{';
        json_add_string(json, "key", def.key, false);
        json_add_string(json, "kind", "paired_enum_numeric");
        json_add_string(json, "label", def.label);
        json_add_string(json, "enum_key", def.enum_key);
        json_add_string(json, "numeric_key", def.numeric_key);

        const As11SettingDef *enum_def = as11_find_setting(def.enum_key);
        const As11SettingDef *numeric_def = as11_find_setting(def.numeric_key);
        const std::string enum_rpc_name =
            enum_def ? as11_setting_rpc_long_name(*enum_def) : def.enum_key;
        const std::string numeric_rpc_name =
            numeric_def ? as11_setting_rpc_long_name(*numeric_def)
                        : def.numeric_key;
        const std::string rpc_name = enum_rpc_name + " + " + numeric_rpc_name;
        json_add_string(json, "rpc_name", rpc_name.c_str());
        json_add_string(json, "enum_rpc_name", enum_rpc_name.c_str());
        json_add_string(json, "numeric_rpc_name", numeric_rpc_name.c_str());
        json_add_int(json, "numeric_branch_enum_value",
                     def.numeric_branch_enum_value);
        json_add_string(json, "group", def.group);
        json_add_string(json, "category", def.category);

        json += ",\"options\":[";
        for (uint8_t option = 0; option < def.option_count; ++option) {
            const As11SettingCompositeOption &item = def.options[option];
            if (option) json += ',';
            json += '{';
            json_add_int(json, "value", option, false);
            json_add_string(json, "label", item.label);
            json_add_int(json, "enum_value", item.enum_value);
            if (item.numeric_raw) {
                json_add_string(json, "numeric_raw", item.numeric_raw);
            }
            json += '}';
        }
        json += "]}";
    }
    json += "]}";
}

bool send_json_buffer(AsyncWebServerRequest *request,
                      const LargeTextBuffer &json) {
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) return false;

    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
    return true;
}

}  // namespace

bool SettingsHttpController::begin(RpcRequestPort &rpc,
                                   As11DeviceService &device,
                                   As11SettingsManager &settings) {
    rpc_ = &rpc;
    device_ = &device;
    settings_ = &settings;

    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!commands_.begin() || !cache_mutex_) return false;

    catalog_json_.reserve(AC_WEB_SETTINGS_CATALOG_JSON_RESERVE);
    build_catalog_json(catalog_json_);
    return !catalog_json_.overflowed();
}

void SettingsHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/settings-catalog"), HTTP_GET,
               [this](AsyncWebServerRequest *request) {
        send_catalog(request);
    });

    server.on(AsyncURIMatcher::exact("/api/settings"), HTTP_GET,
               [this](AsyncWebServerRequest *request) {
        bool refresh_requested = false;
        if (request->hasArg("refresh")) {
            Command command;
            command.kind = CommandKind::Refresh;
            refresh_requested = enqueue(std::move(command));
        }

        send_settings(request, request_profile_mode(request),
                      refresh_requested);
    });

    server.on(
        AsyncURIMatcher::exact("/api/settings"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body) ||
                !doc.is<JsonObject>()) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }

            Command command;
            command.kind = CommandKind::Update;
            command.body = std::move(body);
            const bool queued = enqueue(std::move(command));
            request->send(
                queued ? 202 : 503, "application/json",
                queued ? "{\"ok\":true,\"queued\":true,\"result\":\"queued\"}"
                       : "{\"ok\":false,\"error\":\"queue_full\"}");
        },
        nullptr, http_request_body_handler);
}

void SettingsHttpController::poll() {
    if (!rpc_ || !device_ || !settings_) return;

    drain_commands();
    publish_snapshot_if_needed();
}

bool SettingsHttpController::enqueue(Command &&command) {
    const bool queued = commands_.push(std::move(command));
    if (!queued) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "HTTP settings command queue full\n");
    }
    return queued;
}

void SettingsHttpController::drain_commands() {
    for (size_t i = 0; i < CommandsPerPoll; ++i) {
        Command command;
        if (!commands_.pop(command)) break;

        execute(command);
    }
}

void SettingsHttpController::execute(Command &command) {
    if (command.kind == CommandKind::Refresh) {
        (void)settings_->request_refresh(*rpc_, RpcSource::HttpApi, millis());
    } else {
        const As11SettingsState &state = settings_->state();
        const As11DeviceState &device = device_->state();
        const int mode = active_settings_mode(device, state);

        size_t accepted = 0;
        const std::string params =
            as11_build_set_params_from_json(command.body, mode, accepted);
        if (accepted) {
            (void)settings_->write(
                *rpc_, params, RpcSource::HttpApi, millis());
        }
    }

    if (xSemaphoreTake(cache_mutex_, 0) == pdTRUE) {
        snapshot_pending_ = true;
        xSemaphoreGive(cache_mutex_);
    }
}

void SettingsHttpController::publish_snapshot_if_needed() {
    int requested_mode = -1;
    uint32_t request_generation = 0;
    bool requested_snapshot = false;
    if (xSemaphoreTake(cache_mutex_, 0) == pdTRUE) {
        requested_mode = requested_mode_;
        request_generation = request_generation_;
        requested_snapshot = snapshot_pending_;
        xSemaphoreGive(cache_mutex_);
    } else {
        return;
    }

    const bool refresh_pending = settings_->refresh_pending();
    const uint32_t settings_revision = settings_->revision();
    const uint32_t device_revision = device_->revision();
    if (!requested_snapshot &&
        observed_refresh_pending_ == refresh_pending &&
        observed_settings_revision_ == settings_revision &&
        observed_device_revision_ == device_revision) {
        return;
    }

    LargeTextBuffer next;
    next.reserve(AC_WEB_SETTINGS_JSON_RESERVE);
    build_settings_json(next, device_->state(), settings_->state(),
                        requested_mode, refresh_pending);
    if (next.overflowed()) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "HTTP settings snapshot allocation failed\n");
        return;
    }

    if (xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return;
    if (request_generation_ == request_generation) {
        settings_json_.swap(next);
        cached_request_mode_ = requested_mode;
        cached_refresh_pending_ = refresh_pending;
        snapshot_pending_ = false;
    }
    xSemaphoreGive(cache_mutex_);

    observed_refresh_pending_ = refresh_pending;
    observed_settings_revision_ = settings_revision;
    observed_device_revision_ = device_revision;
}

void SettingsHttpController::send_catalog(
    AsyncWebServerRequest *request) const {
    if (!catalog_json_.length() || catalog_json_.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"catalog unavailable\"}");
        return;
    }
    if (!send_json_buffer(request, catalog_json_)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
    }
}

void SettingsHttpController::send_settings(
    AsyncWebServerRequest *request,
    int requested_mode,
    bool refresh_requested) {
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"valid\":false,\"refresh_queued\":true,"
                      "\"settings\":[]}");
        return;
    }

    if (requested_mode_ != requested_mode ||
        cached_request_mode_ != requested_mode) {
        requested_mode_ = requested_mode;
        request_generation_++;
        snapshot_pending_ = true;
    }

    const bool snapshot_pending = snapshot_pending_;
    const bool refresh_queued =
        refresh_requested || cached_refresh_pending_;
    if (!snapshot_pending && !refresh_queued && settings_json_.length()) {
        AsyncResponseStream *response =
            request->beginResponseStream("application/json");
        if (!response) {
            xSemaphoreGive(cache_mutex_);
            request->send(
                503, "application/json",
                "{\"valid\":false,\"error\":\"response alloc\"}");
            return;
        }

        response->write(
            reinterpret_cast<const uint8_t *>(settings_json_.c_str()),
            settings_json_.length());
        xSemaphoreGive(cache_mutex_);
        request->send(response);
        return;
    }

    xSemaphoreGive(cache_mutex_);
    request->send(200, "application/json",
                  settings_placeholder_json(refresh_queued,
                                            snapshot_pending));
}

}  // namespace aircannect
