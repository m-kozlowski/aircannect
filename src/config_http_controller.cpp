#include "config_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <stdlib.h>
#include <string.h>
#include <utility>

#include "app_config_registry.h"
#include "config_service.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "json_util.h"

namespace aircannect {
namespace {

static constexpr size_t CONFIG_JSON_RESERVE_ALL = 4096;
static constexpr size_t CONFIG_JSON_RESERVE_LARGE_SECTION = 2048;
static constexpr size_t CONFIG_JSON_RESERVE_SYNC_SECTION = 640;
static constexpr size_t CONFIG_JSON_RESERVE_SMALL_SECTION = 384;
static constexpr size_t CONFIG_SCHEMA_JSON_RESERVE = 12 * 1024;

struct ConfigSection {
    const char *path;
    const char *id;
};

static constexpr ConfigSection CONFIG_SECTIONS[] = {
    {"/api/config/device", "device"},
    {"/api/config/network", "network"},
    {"/api/config/access", "access"},
    {"/api/config/ota", "ota"},
    {"/api/config/logging", "logging"},
    {"/api/config/time", "time"},
    {"/api/config/oximetry", "oximetry"},
    {"/api/config/smb", "smb"},
    {"/api/config/sleephq", "sleephq"},
};

static_assert(sizeof(CONFIG_SECTIONS) / sizeof(CONFIG_SECTIONS[0]) == 9,
              "ConfigHttpController section count mismatch");

static constexpr AppConfigEnumValue WEB_LOG_LEVEL_VALUES[] = {
    {"ERROR", "Error"},
    {"WARN", "Warn"},
    {"INFO", "Info"},
    {"DEBUG", "Debug"},
};

size_t section_index(const char *section) {
    constexpr size_t count =
        sizeof(CONFIG_SECTIONS) / sizeof(CONFIG_SECTIONS[0]);
    if (!section || !section[0] || strcmp(section, "all") == 0) {
        return count;
    }

    for (size_t i = 0; i < count; ++i) {
        if (strcmp(section, CONFIG_SECTIONS[i].id) == 0) return i;
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

const char *field_type_name(AppConfigFieldType type) {
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
    }
    return false;
}

void append_schema_enum(LargeTextBuffer &json,
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
        if (!section_includes(section, field.group)) continue;

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
        json_add_string(json, "type", field_type_name(field.type));
        json_add_bool(json, "secret", app_config_field_is_secret(field));
        json_add_bool(json, "provisionable",
                      (field.flags & AC_CONFIG_FIELD_PROVISIONABLE) != 0);
        if (field.help && field.help[0]) {
            json_add_string(json, "help", field.help);
        }
        append_schema_enum(json, field);
        json += '}';
    }

    if (group_open) json += "]}";
    json += "]}";
}

bool write_json_response(AsyncWebServerRequest *request,
                         const LargeTextBuffer &json,
                         AsyncResponseStream *&response) {
    response = request->beginResponseStream("application/json");
    if (!response) return false;

    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    return true;
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
        AsyncURIMatcher::exact("/api/config"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { send_update(request); },
        nullptr, http_request_body_handler);

    for (const ConfigSection &section : CONFIG_SECTIONS) {
        server.on(AsyncURIMatcher::exact(section.path), HTTP_GET,
                  [this, id = section.id](AsyncWebServerRequest *request) {
            send_config(request, id);
        });

        server.on(
            AsyncURIMatcher::exact(section.path), HTTP_POST,
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
    JsonDocument doc;
    if (deserializeJson(doc, command.body.c_str())) return;

    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull() || !config_->begin_transaction()) return;

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
        next_sections[i].reserve(config_json_reserve(CONFIG_SECTIONS[i].id));
        build_config_json(next_sections[i], config_->data(),
                          CONFIG_SECTIONS[i].id);
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
    if (xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache_busy\"}");
        return;
    }

    const LargeTextBuffer &json =
        index == SectionCount ? all_json_ : section_json_[index];
    AsyncResponseStream *response = nullptr;
    const bool prepared = write_json_response(request, json, response);
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
    if (!write_json_response(request, schema_json_, response)) {
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

}  // namespace aircannect
