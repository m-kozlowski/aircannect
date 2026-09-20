#include "settings_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include "http_route_registry.h"

#include <string.h>
#include <utility>

#include "as11_device_service.h"
#include "as11_settings.h"
#include "as11_settings_manager.h"
#include "board.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "http_response_utils.h"
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
        mode = settings.mode_index_from_device_value(
            device.active_therapy_profile());
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

String settings_placeholder_json(As11Availability availability,
                                  bool refresh_queued,
                                  bool snapshot_pending,
                                  uint32_t catalog_revision) {
    String json = "{";
    json_add_bool(json, "valid", false, false);
    json_add_string(json, "as11_state",
                    As11DeviceState::availability_name(availability));
    json_add_bool(json, "refresh_queued", refresh_queued);
    json_add_bool(json, "snapshot_pending", snapshot_pending);
    json_add_int(json, "catalog_revision", catalog_revision);
    json_add_int(json, "pending_count", 0);
    json_add_string(json, "last_write_status", "");
    json += ",\"last_write_age_ms\":null";
    json += ",\"age_ms\":null";
    json += ",\"settings\":[]}";
    return json;
}

void begin_settings_json(LargeTextBuffer &json,
                         const As11DeviceState &device,
                         const As11SettingsState &state,
                         bool refresh_queued,
                         uint32_t now_ms) {
    json = "{";
    json_add_bool(json, "valid", state.valid(), false);
    json_add_string(json, "as11_state",
                    As11DeviceState::availability_name(
                        device.availability()));
    json_add_bool(json, "refresh_queued", refresh_queued);
    json_add_int(json, "catalog_revision", state.catalog().revision());
    json_add_int(json, "supported_mode_mask", state.supported_mode_mask());
    json_add_int(json, "pending_count",
                 static_cast<long>(state.pending_count()));
    json_add_string(json, "last_write_status",
                    state.last_write_status().c_str());
    if (state.last_write_ms()) {
        json_add_int(json, "last_write_age_ms",
                     now_ms - state.last_write_ms());
    } else {
        json += ",\"last_write_age_ms\":null";
    }
    if (state.valid()) {
        json_add_int(json, "age_ms", now_ms - state.updated_ms());
    } else {
        json += ",\"age_ms\":null";
    }

    json += ",\"settings\":[";
}

void append_setting_json(LargeTextBuffer &json,
                         const As11SettingsState &state,
                         size_t index, int active_mode, int profile_mode,
                         uint32_t now_ms, size_t &emitted) {
    const As11SettingDef &def = state.catalog().setting(index);
    if (!state.setting_visible(index, profile_mode) ||
        !as11_setting_readable_via_rpc(def)) {
        return;
    }

    const bool therapy_mode = strcmp(def.key, "MOP") == 0;
    const std::string value =
        state.value(index, therapy_mode ? active_mode : profile_mode);
    const bool pending = state.pending(index);
    if (value.empty() && !pending) return;

    if (emitted++) json += ',';
    json += '{';
    json_add_string(json, "key", def.key, false);
    json_add_string(json, "value", value.c_str());
    if (pending) {
        json_add_bool(json, "pending", true);
        json_add_string(json, "pending_value",
                        state.pending_value(index).c_str());
        json_add_int(json, "pending_age_ms",
                     now_ms - state.pending_since_ms(index));
    }
    json += '}';
}

void append_catalog_setting_json(LargeTextBuffer &json,
                                 const As11SettingsCatalog &catalog,
                                 const As11SettingDef &def,
                                 size_t &emitted) {
    if (!catalog.supports(def) || !def.mode_mask ||
        !as11_setting_readable_via_rpc(def)) {
        return;
    }

    if (emitted++) json += ',';
    json += '{';
    json_add_string(json, "key", def.key, false);
    json_add_string(json, "label", def.label);
    const std::string rpc_name = as11_setting_rpc_long_name(
        def, catalog.device_model());
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
    if (!def.writable) json_add_bool(json, "writable", false);
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

void append_catalog_composite_json(LargeTextBuffer &json,
                                   const As11SettingsCatalog &catalog,
                                   size_t index, size_t &emitted) {
    const As11SettingCompositeDef &def = as11_setting_composite(index);
    const As11SettingDef *enum_def = catalog.find(def.enum_key);
    const As11SettingDef *numeric_def = catalog.find(def.numeric_key);
    if (!enum_def || !numeric_def || !catalog.supports(*enum_def) ||
        !catalog.supports(*numeric_def)) return;

    if (catalog.overlaid(def.enum_key) || catalog.overlaid(def.numeric_key)) {
        return;
    }

    if (emitted++) json += ',';
    json += '{';
    json_add_string(json, "key", def.key, false);
    json_add_string(json, "kind", "paired_enum_numeric");
    json_add_string(json, "label", def.label);
    json_add_string(json, "enum_key", def.enum_key);
    json_add_string(json, "numeric_key", def.numeric_key);

    json_add_string(json, "group", def.group);
    json_add_string(json, "category", def.category);

    const std::string enum_rpc_name =
        as11_setting_rpc_long_name(*enum_def, catalog.device_model());
    const std::string numeric_rpc_name =
        as11_setting_rpc_long_name(*numeric_def, catalog.device_model());
    const std::string rpc_name = enum_rpc_name + " + " + numeric_rpc_name;
    json_add_string(json, "rpc_name", rpc_name.c_str());
    json_add_string(json, "enum_rpc_name", enum_rpc_name.c_str());
    json_add_string(json, "numeric_rpc_name", numeric_rpc_name.c_str());
    json_add_int(json, "numeric_branch_enum_value",
                 def.numeric_branch_enum_value);

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
    if (!commands_.begin() || !cache_mutex_ ||
        !settings_snapshot_.begin(AC_WEB_SETTINGS_JSON_RESERVE)) {
        return false;
    }

    return catalog_json_.reserve(AC_WEB_SETTINGS_CATALOG_JSON_RESERVE) &&
           settings_build_json_.reserve(AC_WEB_SETTINGS_JSON_RESERVE) &&
           catalog_build_json_.reserve(AC_WEB_SETTINGS_CATALOG_JSON_RESERVE);
}

void SettingsHttpController::register_routes(HttpRouteRegistry &server) {
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
    if (device_->unavailable()) {
        if (command.kind == CommandKind::Refresh) {
            (void)device_->request_healthcheck(
                *rpc_, RpcSource::HttpApi, millis());
        }
        return;
    }

    if (command.kind == CommandKind::Refresh) {
        (void)settings_->request_refresh(*rpc_, RpcSource::HttpApi, millis());
    } else {
        const As11SettingsState &state = settings_->state();
        const As11DeviceState &device = device_->state();
        const int mode = active_settings_mode(device, state);

        size_t accepted = 0;
        const std::string params =
            as11_build_set_params_from_json(
                command.body, mode, accepted, state.catalog());
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

    const As11Availability availability = device_->state().availability();
    const bool refresh_pending =
        availability != As11Availability::Unavailable &&
        settings_->refresh_pending();
    const uint32_t settings_revision = settings_->revision();
    const uint32_t device_revision = device_->revision();
    const uint32_t catalog_revision = settings_->state().catalog().revision();
    const int active_mode = active_settings_mode(device_->state(),
                                                  settings_->state());

    // A changing clock/device revision does not invalidate settings being
    // assembled. Only the device fields actually included in the JSON do.
    if (build_.phase != BuildPhase::Idle &&
        (build_.settings_revision != settings_revision ||
         build_.catalog_revision != catalog_revision ||
         build_.request_generation != request_generation ||
         build_.availability != availability ||
         build_.active_mode != active_mode ||
         build_.refresh_pending != refresh_pending)) {
        build_.phase = BuildPhase::Idle;
    }

    if (build_.phase == BuildPhase::Idle && !requested_snapshot &&
        cached_catalog_revision_ == catalog_revision &&
        observed_refresh_pending_ == refresh_pending &&
        observed_settings_revision_ == settings_revision &&
        observed_device_revision_ == device_revision) {
        return;
    }

    if (build_.phase == BuildPhase::Idle) {
        build_ = {};
        build_.phase = BuildPhase::Settings;
        build_.settings_revision = settings_revision;
        build_.catalog_revision = catalog_revision;
        build_.device_revision = device_revision;
        build_.request_generation = request_generation;
        build_.now_ms = millis();
        build_.active_mode = active_mode;
        build_.profile_mode =
            requested_mode >= 0 ? requested_mode : active_mode;
        build_.availability = availability;
        build_.refresh_pending = refresh_pending;
        build_.advance_revision =
            requested_snapshot ||
            observed_refresh_pending_ != refresh_pending ||
            observed_settings_revision_ != settings_revision ||
            published_availability_ != availability ||
            published_active_mode_ != active_mode;

        const uint16_t supported_modes =
            settings_->state().supported_mode_mask();

        if (build_.profile_mode >= 0 && supported_modes &&
            !(supported_modes & (1u << build_.profile_mode))) {
            build_.profile_mode = active_mode;
        }

        begin_settings_json(settings_build_json_, device_->state(),
                            settings_->state(), refresh_pending,
                            build_.now_ms);

        catalog_build_json_.clear();
    }

    advance_snapshot_build();
    if (settings_build_json_.overflowed() || catalog_build_json_.overflowed()) {
        build_.phase = BuildPhase::Idle;
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "HTTP settings snapshot allocation failed\n");
        return;
    }
    if (build_.phase != BuildPhase::Ready) return;

    if (xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return;
    if (request_generation_ == build_.request_generation &&
        settings_snapshot_.replace(settings_build_json_,
                                   build_.advance_revision)) {
        if (cached_catalog_revision_ != build_.catalog_revision) {
            catalog_json_.swap(catalog_build_json_);
            cached_catalog_revision_ = build_.catalog_revision;
        }

        cached_device_availability_ = build_.availability;
        cached_refresh_pending_ = build_.refresh_pending;
        snapshot_pending_ = false;
        published_availability_ = build_.availability;
        published_active_mode_ = build_.active_mode;
        observed_refresh_pending_ = build_.refresh_pending;
        observed_settings_revision_ = build_.settings_revision;
        observed_device_revision_ = build_.device_revision;
        build_.phase = BuildPhase::Idle;
    }
    xSemaphoreGive(cache_mutex_);
}

void SettingsHttpController::advance_snapshot_build() {
    const As11SettingsState &state = settings_->state();
    const As11SettingsCatalog &catalog = state.catalog();
    const uint32_t started_us = micros();

    for (size_t count = 0; count < SnapshotItemsPerPoll; ++count) {
        switch (build_.phase) {
            case BuildPhase::Settings:
                if (build_.index < catalog.count()) {
                    append_setting_json(settings_build_json_, state,
                                        build_.index++,
                                        build_.active_mode, build_.profile_mode,
                                        build_.now_ms, build_.emitted);

                    break;
                }

                settings_build_json_ += "]}";
                if (cached_catalog_revision_ == build_.catalog_revision) {
                    build_.phase = BuildPhase::Ready;
                    return;
                }

                catalog_build_json_ = "{";
                json_add_int(catalog_build_json_, "revision",
                             build_.catalog_revision, false);

                catalog_build_json_ += ",\"settings\":[";
                build_.phase = BuildPhase::Catalog;
                build_.index = build_.emitted = 0;
                break;

            case BuildPhase::Catalog:
                if (build_.index < catalog.count()) {
                    append_catalog_setting_json(catalog_build_json_,
                                                catalog,
                                                catalog.setting(build_.index++),
                                                build_.emitted);

                    break;
                }

                catalog_build_json_ += "],\"composites\":[";
                build_.phase = BuildPhase::Composites;
                build_.index = build_.emitted = 0;
                break;

            case BuildPhase::Composites:
                if (build_.index < as11_setting_composite_count()) {
                    append_catalog_composite_json(catalog_build_json_, catalog,
                                                  build_.index++, build_.emitted);

                    break;
                }

                catalog_build_json_ += "]}";
                build_.phase = BuildPhase::Ready;
                return;

            case BuildPhase::Idle:
            case BuildPhase::Ready:
                return;
        }

        if (static_cast<uint32_t>(micros() - started_us) >= SnapshotBudgetUs) return;
    }
}

void SettingsHttpController::send_catalog(
    AsyncWebServerRequest *request) {
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"catalog busy\"}");
        return;
    }
    if (!catalog_json_.length() || catalog_json_.overflowed()) {
        xSemaphoreGive(cache_mutex_);
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"catalog unavailable\"}");
        return;
    }

    AsyncWebServerResponse *response = nullptr;
    const bool prepared =
        http_prepare_json_response(request, catalog_json_, response);
    xSemaphoreGive(cache_mutex_);

    if (!prepared) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
        return;
    }

    request->send(response);
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

    if (requested_mode_ != requested_mode) {
        requested_mode_ = requested_mode;
        request_generation_++;
        snapshot_pending_ = true;
    }

    const bool snapshot_pending = snapshot_pending_;
    const As11Availability availability = cached_device_availability_;
    const bool unavailable =
        availability == As11Availability::Unavailable;
    const bool refresh_queued =
        !unavailable && (refresh_requested || cached_refresh_pending_);
    const uint32_t catalog_revision = cached_catalog_revision_;
    if (!snapshot_pending && !refresh_queued &&
        settings_snapshot_.revision() != 0) {
        AsyncWebServerResponse *response = nullptr;
        const JsonSnapshotResponse result =
            settings_snapshot_.prepare_response(request, response);
        if (result != JsonSnapshotResponse::Ready) {
            xSemaphoreGive(cache_mutex_);
            request->send(
                503, "application/json",
                "{\"valid\":false,\"error\":\"response alloc\"}");
            return;
        }

        xSemaphoreGive(cache_mutex_);
        request->send(response);
        return;
    }

    xSemaphoreGive(cache_mutex_);
    request->send(200, "application/json",
                  settings_placeholder_json(availability,
                                            refresh_queued,
                                            snapshot_pending,
                                            catalog_revision));
}

}  // namespace aircannect
