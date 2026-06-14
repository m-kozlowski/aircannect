#include "edf_str_settings.h"

#include <ArduinoJson.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "edf_str_field_map.h"

namespace aircannect {
namespace {

bool parse_float_text(const char *text, float &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const float value = strtof(text, &end);
    if (!end || *end != 0 || !isfinite(value)) return false;
    out = value;
    return true;
}

bool parse_iso8601_seconds(const char *text, float &out) {
    if (!text || text[0] != 'P' || text[1] != 'T') return false;
    const char *value = text + 2;
    char *end = nullptr;
    const float seconds = strtof(value, &end);
    if (!end || *end != 'S' || end[1] != 0 || !isfinite(seconds)) {
        return false;
    }
    out = seconds;
    return true;
}

void rpc_name_for_str_tag(const char *tag, char *out, size_t out_size) {
    if (!out || !out_size) return;
    out[0] = 0;
    if (!tag || !tag[0]) return;
    if (tag[0] == '_') {
        snprintf(out, out_size, "%s", tag);
        return;
    }
    snprintf(out, out_size, "_%s", tag);
}

bool physical_from_json_value(JsonVariantConst value,
                              const char *rpc_name,
                              float &physical) {
    if (value.is<bool>()) {
        physical = value.as<bool>() ? 1.0f : 0.0f;
        return true;
    }
    if (value.is<float>() || value.is<int>() || value.is<long>()) {
        physical = value.as<float>();
        return true;
    }
    if (!value.is<const char *>()) return false;

    const char *text = value.as<const char *>();
    int16_t option_index = 0;
    if (parse_iso8601_seconds(text, physical) ||
        parse_float_text(text, physical)) {
        return true;
    }
    if (as11_setting_option_index_for_rpc_name(rpc_name, text, option_index)) {
        physical = static_cast<float>(option_index);
        return true;
    }
    return false;
}

}  // namespace

std::string edf_str_setting_get_names() {
    std::string names;
    names.reserve(512);
    for (size_t i = 0; i < AC_EDF_STR_FIELD_MAP_COUNT; ++i) {
        const EdfStrFieldMap &field = AC_EDF_STR_FIELD_MAP[i];
        if (field.source != EdfStrFieldSource::SettingGet ||
            !field.short_tag) {
            continue;
        }

        char rpc_name[8] = {};
        rpc_name_for_str_tag(field.short_tag, rpc_name, sizeof(rpc_name));
        if (!rpc_name[0]) continue;
        if (!names.empty()) names += ' ';
        names += rpc_name;
    }
    return names;
}

bool edf_str_apply_settings_response(const std::string &payload,
                                     EdfStrSessionAccumulator &session,
                                     EdfStrSettingsApplyResult &result) {
    result = {};

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        result.error = "str_settings_json_failed";
        return false;
    }

    JsonObjectConst json_result = doc["result"].as<JsonObjectConst>();
    if (json_result.isNull()) {
        result.error = json_member_present(payload, "error")
                           ? "str_settings_rpc_error"
                           : "str_settings_missing_result";
        return false;
    }

    for (size_t i = 0; i < AC_EDF_STR_FIELD_MAP_COUNT; ++i) {
        const EdfStrFieldMap &field = AC_EDF_STR_FIELD_MAP[i];
        if (field.source != EdfStrFieldSource::SettingGet ||
            !field.short_tag) {
            continue;
        }

        char rpc_name[8] = {};
        rpc_name_for_str_tag(field.short_tag, rpc_name, sizeof(rpc_name));
        JsonVariantConst value = json_result[rpc_name];
        if (value.isNull()) {
            result.missing++;
            continue;
        }

        float physical = 0.0f;
        if (physical_from_json_value(value, rpc_name, physical) &&
            session.set_signal_physical(i, physical)) {
            result.values++;
        } else {
            result.unmapped++;
        }
    }

    result.ok = true;
    return true;
}

}  // namespace aircannect
