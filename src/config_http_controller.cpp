#include "config_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <utility>

#include "app_config_registry.h"
#include "board_button.h"
#include "button_binding_config.h"
#include "config_service.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "http_response_utils.h"
#include "json_util.h"

namespace aircannect {
namespace {

static constexpr size_t CONFIG_JSON_RESERVE_ALL = 4096;
static constexpr size_t CONFIG_JSON_RESERVE_LARGE_SECTION = 2048;
static constexpr size_t CONFIG_JSON_RESERVE_SYNC_SECTION = 640;
static constexpr size_t CONFIG_JSON_RESERVE_SMALL_SECTION = 384;
static constexpr size_t CONFIG_SCHEMA_JSON_RESERVE = 12 * 1024;

size_t section_index(const char *section) {
    constexpr size_t count = AC_CONFIG_GROUP_COUNT;
    if (!section || !section[0] || strcmp(section, "all") == 0) {
        return count;
    }

    for (size_t i = 0; i < count; ++i) {
        const AppConfigGroup group = static_cast<AppConfigGroup>(i);
        if (strcmp(section, app_config_group_id(group)) == 0) return i;
    }
    return count + 1;
}

bool section_includes(const char *section, AppConfigGroup group) {
    return !section || !section[0] || strcmp(section, "all") == 0 ||
           strcmp(section, app_config_group_id(group)) == 0;
}

size_t config_json_reserve(const char *section) {
    if (!section || !section[0] || strcmp(section, "all") == 0) {
        return CONFIG_JSON_RESERVE_ALL;
    }
    if (strcmp(section, "access") == 0 ||
        strcmp(section, "logging") == 0) {
        return CONFIG_JSON_RESERVE_LARGE_SECTION;
    }
    if (strcmp(section, "smb") == 0 ||
        strcmp(section, "sleephq") == 0) {
        return CONFIG_JSON_RESERVE_SYNC_SECTION;
    }
    return CONFIG_JSON_RESERVE_SMALL_SECTION;
}

bool config_value_text(JsonVariantConst value,
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
        case AppConfigFieldType::Keybindings:
            return false;
    }
    return false;
}

bool known_binding(const ButtonBinding &binding) {
    return board_button_input_supported(binding.input, binding.gesture) &&
           local_action_find(binding.action) != nullptr;
}

void append_keybindings_json(LargeTextBuffer &json,
                             const ButtonBindingConfig &config,
    bool comma) {
    if (comma) json += ',';
    json += "\"keybindings\":{";
    json_add_string(json, "state",
                    button_binding_blob_state_name(config.state), false);
    json += ",\"overrides\":[";

    bool first = true;
    if (config.state == ButtonBindingBlobState::Valid) {
        for (const ButtonBinding &binding : config.overrides) {
            if (!known_binding(binding)) continue;

            const LocalActionDefinition *action =
                local_action_find(binding.action);
            char input_id[48] = {};
            if (!action || !board_button_input_id(
                               binding.input, input_id,
                               sizeof(input_id))) {
                continue;
            }

            if (!first) json += ',';
            first = false;
            json += '{';
            json_add_string(json, "button", input_id, false);
            json_add_string(json, "gesture",
                            button_gesture_name(binding.gesture));
            json_add_string(json, "action", action->name);
            json += '}';
        }
    }
    json += "]}";
}

void append_keybinding_input_schema(LargeTextBuffer &json,
                                    ButtonInput input,
                                    const ButtonBinding *defaults,
                                    size_t default_count,
                                    bool &first_input) {
    char id[48] = {};
    char label[64] = {};
    if (!board_button_input_id(input, id, sizeof(id)) ||
        !board_button_input_label(input, label, sizeof(label))) {
        return;
    }

    if (!first_input) json += ',';
    first_input = false;
    json += '{';
    json_add_string(json, "id", id, false);
    if (!input.chord()) {
        json_add_int(json, "key", input.first_button_key);
    }
    json_add_string(json, "label", label);
    json += ",\"gestures\":[";

    bool first_gesture = true;
    const ButtonGesture gestures[] = {
        ButtonGesture::ShortPress,
        ButtonGesture::LongPress,
    };
    for (ButtonGesture gesture : gestures) {
        if (!board_button_input_supported(input, gesture)) continue;

        const ButtonBinding *profile_default = button_binding_find(
            defaults, default_count, input, gesture);
        const LocalActionId default_action =
            profile_default ? profile_default->action
                            : LocalActionId::None;
        const LocalActionDefinition *action =
            local_action_find(default_action);

        if (!first_gesture) json += ',';
        first_gesture = false;
        json += '{';
        json_add_string(json, "gesture",
                        button_gesture_name(gesture), false);
        json_add_string(json, "default",
                        action ? action->name : "none");
        json += '}';
    }
    json += "]}";
}

void append_keybindings_schema(LargeTextBuffer &json) {
    json += ",\"buttons\":[";

    size_t button_count = 0;
    const BoardButtonDefinition *buttons = board_button_catalog(button_count);
    size_t default_count = 0;
    const ButtonBinding *defaults =
        board_button_binding_defaults(default_count);
    bool first_input = true;
    for (size_t i = 0; i < button_count; ++i) {
        append_keybinding_input_schema(
            json, button_input(buttons[i].key), defaults, default_count,
            first_input);
    }

    for (size_t i = 0; i < default_count; ++i) {
        if (!defaults[i].input.chord()) continue;

        bool already_added = false;
        for (size_t previous = 0; previous < i; ++previous) {
            if (defaults[previous].input == defaults[i].input) {
                already_added = true;
                break;
            }
        }
        if (already_added) continue;

        append_keybinding_input_schema(
            json, defaults[i].input, defaults, default_count,
            first_input);
    }
    json += "]";

    json += ",\"actions\":[";
    size_t action_count = 0;
    const LocalActionDefinition *actions =
        local_action_catalog(action_count);
    for (size_t i = 0; i < action_count; ++i) {
        if (i) json += ',';
        json += '{';
        json_add_string(json, "value", actions[i].name, false);
        json_add_string(json, "label", actions[i].label);
        json += '}';
    }
    json += ']';
}

bool parse_keybindings_update(JsonVariantConst value,
                              const ButtonBindingConfig &current,
                              ButtonBindingConfig &next) {
    if (!value.is<JsonObjectConst>()) return false;
    const JsonObjectConst object = value.as<JsonObjectConst>();

    if (object["reset"].is<bool>() && object["reset"].as<bool>()) {
        button_binding_reset(next);
        return true;
    }
    if (current.state != ButtonBindingBlobState::Valid ||
        !object["overrides"].is<JsonArrayConst>()) {
        return false;
    }

    next = current;
    next.overrides.erase(
        std::remove_if(next.overrides.begin(), next.overrides.end(),
                       [](const ButtonBinding &binding) {
                           return known_binding(binding);
                       }),
        next.overrides.end());

    for (JsonVariantConst item : object["overrides"].as<JsonArrayConst>()) {
        if (!item.is<JsonObjectConst>()) return false;
        const JsonObjectConst binding = item.as<JsonObjectConst>();
        if (!binding["button"].is<const char *>() ||
            !binding["gesture"].is<const char *>() ||
            !binding["action"].is<const char *>()) {
            return false;
        }

        ButtonInput input;
        ButtonGesture gesture = ButtonGesture::ShortPress;
        const LocalActionDefinition *action =
            local_action_find(binding["action"].as<const char *>());
        if (!board_button_input_find(
                binding["button"].as<const char *>(), input) ||
            !parse_button_gesture(
                binding["gesture"].as<const char *>(), gesture) ||
            !board_button_input_supported(input, gesture) || !action) {
            return false;
        }

        for (const ButtonBinding &existing : next.overrides) {
            if (existing.input == input &&
                existing.gesture == gesture && known_binding(existing)) {
                return false;
            }
        }
        if (!button_binding_set_override(
                next, input, gesture, action->id)) {
            return false;
        }
    }
    return true;
}

void append_schema_enum(LargeTextBuffer &json,
                        const AppConfigFieldDescriptor &field) {
    if (field.type != AppConfigFieldType::Enum &&
        field.type != AppConfigFieldType::LogLevel) {
        return;
    }

    size_t count = 0;
    const AppConfigEnumValue *values =
        app_config_field_allowed_values(field, count);
    if (!values || !count) return;

    json += ",\"enum\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i) json += ',';
        json += '{';
        json_add_string(json, "value", values[i].value, false);
        json_add_string(json, "label", values[i].label);
        json += '}';
    }
    json += ']';
}

void build_config_json(LargeTextBuffer &json,
                       const AppConfigData &config,
                       const char *section) {
    json = "{";
    bool comma = false;

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        if (!app_config_field_is_user_visible(field) ||
            !section_includes(section, field.group)) {
            continue;
        }

        String value;
        if (!app_config_field_get_raw_value(config, field, value)) continue;

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
                json_add_int(json, field.key,
                             strtol(value.c_str(), nullptr, 10), comma);
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
            case AppConfigFieldType::Keybindings:
                append_keybindings_json(json, config.keybindings, comma);
                comma = true;
                break;
        }
    }
    json += '}';
}

void build_schema_json(LargeTextBuffer &json) {
    json = "{\"groups\":[";

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    AppConfigGroup current_group = AppConfigGroup::Device;
    bool group_open = false;
    bool first_group = true;
    bool first_field = true;

    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        if (!app_config_field_is_user_visible(field)) continue;
        if (!group_open || field.group != current_group) {
            if (group_open) json += "]}";
            if (!first_group) json += ',';
            first_group = false;
            current_group = field.group;
            json += '{';
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
        json += '{';
        json_add_string(json, "key", field.key, false);
        json_add_string(json, "label", field.label);
        json_add_string(json, "type",
                        app_config_field_type_name(field.type));
        json_add_bool(json, "secret", app_config_field_is_secret(field));
        json_add_bool(json, "provisionable",
                      (field.flags & AC_CONFIG_FIELD_PROVISIONABLE) != 0);
        if (field.help && field.help[0]) {
            json_add_string(json, "help", field.help);
        }
        append_schema_enum(json, field);
        if (field.type == AppConfigFieldType::Keybindings) {
            append_keybindings_schema(json);
        }
        json += '}';
    }

    if (group_open) json += "]}";
    json += "]}";
}

}  // namespace

bool ConfigHttpController::begin(ConfigService &config) {
    config_ = &config;
    if (!commands_.begin()) return false;

    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!cache_mutex_) return false;

    schema_json_.reserve(CONFIG_SCHEMA_JSON_RESERVE);
    build_schema_json(schema_json_);
    if (schema_json_.overflowed()) return false;

    return publish_snapshots();
}

void ConfigHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/config"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_config(request, nullptr);
    });

    server.on(AsyncURIMatcher::exact("/api/config/schema"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_schema(request);
    });

    server.on(
        AsyncURIMatcher::exact("/api/onboarding"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_onboarding_complete(request);
        },
        nullptr, http_request_body_handler);

    server.on(
        AsyncURIMatcher::exact("/api/config"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { send_update(request); },
        nullptr, http_request_body_handler);

    for (size_t i = 0; i < SectionCount; ++i) {
        const AppConfigGroup group = static_cast<AppConfigGroup>(i);
        String path = "/api/config/";
        path += app_config_group_id(group);

        server.on(
            AsyncURIMatcher::exact(path), HTTP_GET,
            [this, group](AsyncWebServerRequest *request) {
                send_config(request, app_config_group_id(group));
            });

        server.on(
            AsyncURIMatcher::exact(path), HTTP_POST,
            [this](AsyncWebServerRequest *request) { send_update(request); },
            nullptr, http_request_body_handler);
    }
}

void ConfigHttpController::poll() {
    if (!config_) return;

    for (size_t i = 0; i < CommandsPerPoll; ++i) {
        Command command;
        if (!commands_.pop(command)) break;
        execute(command);
    }

    if (published_revision_ != config_->revision()) {
        (void)publish_snapshots();
    }
}

bool ConfigHttpController::enqueue(Command &&command) {
    const bool queued = commands_.push(std::move(command));
    if (!queued) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "HTTP config command queue full\n");
    }
    return queued;
}

void ConfigHttpController::execute(Command &command) {
    if (command.kind == CommandKind::CompleteOnboarding) {
        const char *http_user = command.onboarding_user_set
                                    ? command.onboarding_user.c_str()
                                    : nullptr;
        const char *http_password = command.onboarding_password_set
                                        ? command.onboarding_password.c_str()
                                        : nullptr;

        if (!config_->complete_onboarding(http_user, http_password)) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "failed to persist onboarding completion\n");
        }
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, command.body.c_str())) return;

    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull() || !config_->begin_transaction()) return;

    for (JsonPairConst pair : root) {
        const char *key = pair.key().c_str();
        const AppConfigFieldDescriptor *field = app_config_find_field(key);
        if (!field || !app_config_field_is_user_visible(*field)) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=unknown\n",
                      key ? key : "<null>");
            continue;
        }

        String value;
        if (field->type == AppConfigFieldType::Keybindings) {
            ButtonBindingConfig keybindings;
            if (!parse_keybindings_update(
                    pair.value(), config_->data().keybindings,
                    keybindings) ||
                !config_->set_transaction_keybindings(
                    keybindings).accepted()) {
                Log::logf(CAT_CONFIG, LOG_WARN,
                          "rejected web config key=%s reason=invalid_value\n",
                          field->key);
            }
            continue;
        }

        if (!config_value_text(pair.value(), *field, value)) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=bad_type\n",
                      field->key);
            continue;
        }

        const ConfigFieldUpdate update =
            config_->set_transaction_value(field->key, value, true);
        if (!update.accepted()) {
            Log::logf(CAT_CONFIG, LOG_WARN,
                      "rejected web config key=%s reason=invalid_value\n",
                      field->key);
        }
    }

    const ConfigTransactionResult result = config_->commit_transaction();
    if (!result.persisted) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "failed to persist one or more web config values\n");
    }
}

bool ConfigHttpController::publish_snapshots() {
    const uint32_t revision = config_->revision();
    LargeTextBuffer next_all;
    LargeTextBuffer next_sections[SectionCount];

    next_all.reserve(config_json_reserve(nullptr));
    build_config_json(next_all, config_->data(), nullptr);
    if (next_all.overflowed()) return false;

    for (size_t i = 0; i < SectionCount; ++i) {
        const AppConfigGroup group = static_cast<AppConfigGroup>(i);
        const char *section = app_config_group_id(group);

        next_sections[i].reserve(config_json_reserve(section));
        build_config_json(next_sections[i], config_->data(), section);
        if (next_sections[i].overflowed()) return false;
    }

    if (xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return false;
    all_json_.swap(next_all);
    for (size_t i = 0; i < SectionCount; ++i) {
        section_json_[i].swap(next_sections[i]);
    }
    published_revision_ = revision;
    xSemaphoreGive(cache_mutex_);
    return true;
}

void ConfigHttpController::send_config(AsyncWebServerRequest *request,
                                       const char *section) const {
    const size_t index = section_index(section);
    if (index > SectionCount) {
        request->send(404, "application/json",
                      "{\"ok\":false,\"error\":\"unknown_section\"}");
        return;
    }

    const LargeTextBuffer &json =
        index == SectionCount ? all_json_ : section_json_[index];
    send_snapshot(request, json);
}

void ConfigHttpController::send_snapshot(
    AsyncWebServerRequest *request, const LargeTextBuffer &json) const {
    if (xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache_busy\"}");
        return;
    }

    AsyncResponseStream *response = nullptr;
    const bool prepared = http_prepare_json_response(request, json, response);
    xSemaphoreGive(cache_mutex_);
    if (!prepared) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    request->send(response);
}

void ConfigHttpController::send_schema(
    AsyncWebServerRequest *request) const {
    AsyncResponseStream *response = nullptr;
    if (!http_prepare_json_response(request, schema_json_, response)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    request->send(response);
}

void ConfigHttpController::send_update(AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!http_parse_json_body(request, doc, body) || !doc.is<JsonObject>()) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }

    Command command;
    command.body = std::move(body);
    const bool queued = enqueue(std::move(command));
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

void ConfigHttpController::send_onboarding_complete(
    AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!http_parse_json_body(request, doc, body) || !doc.is<JsonObject>()) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (!root["http_user"].isNull() &&
        !root["http_user"].is<const char *>()) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad user\"}");
        return;
    }
    if (!root["http_password"].isNull() &&
        !root["http_password"].is<const char *>()) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad password\"}");
        return;
    }

    Command command;
    command.kind = CommandKind::CompleteOnboarding;
    if (root["http_user"].is<const char *>()) {
        command.onboarding_user = root["http_user"].as<const char *>();
        command.onboarding_user_set = true;
    }
    if (root["http_password"].is<const char *>()) {
        command.onboarding_password =
            root["http_password"].as<const char *>();
        command.onboarding_password_set = true;
    }
    const bool queued = enqueue(std::move(command));
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

}  // namespace aircannect
