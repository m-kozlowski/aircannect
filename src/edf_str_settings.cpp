#include "edf_str_settings.h"

#include <ArduinoJson.h>

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "edf_str_signal_table.h"

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

bool rpc_name_uses_msec_physical(const char *rpc_name) {
    return rpc_name &&
           (strcmp(rpc_name, "_Z10") == 0 ||
            strcmp(rpc_name, "_XAA") == 0 ||
            strcmp(rpc_name, "_XB7") == 0);
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
    if (parse_iso8601_seconds(text, physical)) {
        if (rpc_name_uses_msec_physical(rpc_name)) physical *= 1000.0f;
        return true;
    }
    if (parse_float_text(text, physical)) {
        return true;
    }
    if (as11_setting_option_index_for_rpc_name(rpc_name, text, option_index)) {
        physical = static_cast<float>(option_index);
        return true;
    }
    return false;
}

enum class EdfStrNumericInput : uint8_t {
    OptionIndex,
    DigitalCode,
};

struct EdfStrDigitalRemap {
    const char *rpc_name = nullptr;
    const int16_t *codes = nullptr;
    size_t code_count = 0;
    EdfStrNumericInput numeric_input = EdfStrNumericInput::OptionIndex;
};

static constexpr int16_t STR_MODE_CODES[] = {
        3,  // CPAP
        1,  // AutoSet
        2,  // AutoSet For Her
        4,  // S
        10, // ST
        16, // T
        8,  // VAuto
        6,  // ASV
        7,  // ASVAuto
        5,  // iVAPS
        9,  // PAC
};
static constexpr int16_t STR_SENSITIVITY_CODES[] = {1, 2, 3, 4, 5, 6, 7};
static constexpr int16_t STR_BOOL_CODES[] = {1, 2};
static constexpr int16_t STR_THREE_OPTION_CODES[] = {1, 2, 3};
static constexpr int16_t STR_SPONT_RESP_RATE_ENABLE_CODES[] = {1, 3};
static constexpr int16_t STR_TUBE_TYPE_CODES[] = {3, 4, 1};

// AS11/ResScan STR enum export maps. Labels resolve through the setting option
// table; numeric DigitalCode values are already STR-native. Some tags are
// proven by firmware maps, others by native EDF comparison for the current AS11
// setting strings.
static constexpr EdfStrDigitalRemap STR_DIGITAL_REMAPS[] = {
    {"_MOP", STR_MODE_CODES,
     sizeof(STR_MODE_CODES) / sizeof(STR_MODE_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_XE6", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XE7", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_Z11", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_Z12", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_ZU1", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XAB", STR_SENSITIVITY_CODES,
     sizeof(STR_SENSITIVITY_CODES) / sizeof(STR_SENSITIVITY_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_AFC", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_ZZ4", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_ZZ9", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_Z16", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XAM", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XB6", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XA9", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_XB9", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_ZZ5", STR_SPONT_RESP_RATE_ENABLE_CODES,
     sizeof(STR_SPONT_RESP_RATE_ENABLE_CODES) /
         sizeof(STR_SPONT_RESP_RATE_ENABLE_CODES[0]),
     EdfStrNumericInput::DigitalCode},
    {"_RMA", STR_THREE_OPTION_CODES,
     sizeof(STR_THREE_OPTION_CODES) / sizeof(STR_THREE_OPTION_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_EPA", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_EPX", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_EPT", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_SST", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_ABF", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_TBT", STR_TUBE_TYPE_CODES,
     sizeof(STR_TUBE_TYPE_CODES) / sizeof(STR_TUBE_TYPE_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_CCO", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_HMX", STR_BOOL_CODES,
     sizeof(STR_BOOL_CODES) / sizeof(STR_BOOL_CODES[0]),
     EdfStrNumericInput::OptionIndex},
    {"_HTX", STR_THREE_OPTION_CODES,
     sizeof(STR_THREE_OPTION_CODES) / sizeof(STR_THREE_OPTION_CODES[0]),
     EdfStrNumericInput::OptionIndex},
};

bool parse_integer_text(const char *text, int16_t &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    if (!end || *end != 0 || value < INT16_MIN || value > INT16_MAX) {
        return false;
    }
    out = static_cast<int16_t>(value);
    return true;
}

bool integer_from_json_value(JsonVariantConst value, int16_t &out) {
    if (value.is<int>() || value.is<long>()) {
        const long parsed = value.as<long>();
        if (parsed < INT16_MIN || parsed > INT16_MAX) return false;
        out = static_cast<int16_t>(parsed);
        return true;
    }
    if (value.is<float>()) {
        const float parsed = value.as<float>();
        if (!isfinite(parsed) || parsed < static_cast<float>(INT16_MIN) ||
            parsed > static_cast<float>(INT16_MAX)) {
            return false;
        }
        const long rounded = lroundf(parsed);
        if (fabsf(parsed - static_cast<float>(rounded)) > 0.0001f) {
            return false;
        }
        out = static_cast<int16_t>(rounded);
        return true;
    }
    return false;
}

bool text_equals(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

bool str_text_digital_from_json_value(JsonVariantConst value,
                                      const char *rpc_name,
                                      int16_t &digital) {
    if (!text_equals(rpc_name, "_MSK") || !value.is<const char *>()) {
        return false;
    }

    const char *text = value.as<const char *>();
    if (text_equals(text, "Pillows")) {
        digital = 2;
        return true;
    }
    return false;
}

bool str_text_digital_map_is_exclusive(const char *rpc_name) {
    return text_equals(rpc_name, "_MSK");
}

bool fallback_option_index_from_text(const char *text, int16_t &index) {
    if (text_equals(text, "Off") || text_equals(text, "No") ||
        text_equals(text, "Min")) {
        index = 0;
        return true;
    }
    if (text_equals(text, "On") || text_equals(text, "Yes")) {
        index = 1;
        return true;
    }
    return false;
}

bool option_index_from_text(const char *text,
                            const char *rpc_name,
                            int16_t &index) {
    if (as11_setting_option_index_for_rpc_name(rpc_name, text, index)) {
        return true;
    }
    if (strcmp(rpc_name, "_MOP") == 0) {
        const int parsed = as11_mode_index_from_value(text);
        if (parsed >= 0 && parsed <= 10) {
            index = static_cast<int16_t>(parsed);
            return true;
        }
    }
    return fallback_option_index_from_text(text, index);
}

const EdfStrDigitalRemap *str_digital_remap_for_rpc_name(
    const char *rpc_name) {
    if (!rpc_name) return nullptr;
    for (const EdfStrDigitalRemap &remap : STR_DIGITAL_REMAPS) {
        if (strcmp(remap.rpc_name, rpc_name) == 0) return &remap;
    }
    return nullptr;
}

bool digital_from_option_index(const EdfStrDigitalRemap &remap,
                               int16_t option_index,
                               int16_t &digital_value) {
    if (option_index < 0 ||
        option_index >= static_cast<int16_t>(remap.code_count)) {
        return false;
    }

    digital_value = remap.codes[option_index];
    return true;
}

bool digital_code_is_allowed(const EdfStrDigitalRemap &remap,
                             int16_t code) {
    for (size_t i = 0; i < remap.code_count; ++i) {
        if (remap.codes[i] == code) return true;
    }
    return false;
}

bool str_digital_from_json_value(JsonVariantConst value,
                                 const char *rpc_name,
                                 const EdfStrDigitalRemap &remap,
                                 int16_t &digital_value) {
    if (value.is<bool>()) {
        return digital_from_option_index(remap,
                                         value.as<bool>() ? 1 : 0,
                                         digital_value);
    }

    int16_t numeric_value = 0;
    if (integer_from_json_value(value, numeric_value)) {
        if (remap.numeric_input == EdfStrNumericInput::DigitalCode) {
            if (!digital_code_is_allowed(remap, numeric_value)) return false;
            digital_value = numeric_value;
            return true;
        }
        return digital_from_option_index(remap, numeric_value, digital_value);
    }

    if (!value.is<const char *>()) return false;

    const char *text = value.as<const char *>();
    if (parse_integer_text(text, numeric_value)) {
        if (remap.numeric_input == EdfStrNumericInput::DigitalCode) {
            if (!digital_code_is_allowed(remap, numeric_value)) return false;
            digital_value = numeric_value;
            return true;
        }
        return digital_from_option_index(remap, numeric_value, digital_value);
    }

    int16_t option_index = 0;
    if (!option_index_from_text(text, rpc_name, option_index)) return false;
    return digital_from_option_index(remap, option_index, digital_value);
}

JsonObjectConst get_value_object(const JsonDocument &doc) {
    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (!result.isNull()) return result;
    return doc["error"]["data"].as<JsonObjectConst>();
}

bool append_str_get_name(std::string &names, const char *tag) {
    char rpc_name[8] = {};
    rpc_name_for_str_tag(tag, rpc_name, sizeof(rpc_name));
    if (!rpc_name[0]) return false;
    if (!names.empty()) names += ' ';
    names += rpc_name;
    return true;
}

bool summary_code_from_index(JsonVariantConst value,
                             const int16_t *codes,
                             size_t code_count,
                             int16_t &digital) {
    int16_t index = 0;
    if (integer_from_json_value(value, index) ||
        (value.is<const char *>() &&
         parse_integer_text(value.as<const char *>(), index))) {
        if (index < 0 || index >= static_cast<int16_t>(code_count)) {
            return false;
        }
        digital = codes[index];
        return true;
    }
    return false;
}

bool summary_tube_connected_code(JsonVariantConst value, int16_t &digital) {
    static constexpr int16_t kCodes[] = {3, 4, 1, 5, 2};
    if (summary_code_from_index(value,
                                kCodes,
                                sizeof(kCodes) / sizeof(kCodes[0]),
                                digital)) {
        return true;
    }
    if (!value.is<const char *>()) return false;
    const char *text = value.as<const char *>();
    if (text_equals(text, "15mmNonHeated")) {
        digital = kCodes[0];
        return true;
    }
    if (text_equals(text, "19mm")) {
        digital = kCodes[1];
        return true;
    }
    if (text_equals(text, "15mmHeated")) {
        digital = kCodes[2];
        return true;
    }
    return false;
}

bool summary_humidifier_connected_code(JsonVariantConst value,
                                       int16_t &digital) {
    static constexpr int16_t kCodes[] = {1, 2, 3};
    if (summary_code_from_index(value,
                                kCodes,
                                sizeof(kCodes) / sizeof(kCodes[0]),
                                digital)) {
        return true;
    }
    if (!value.is<const char *>()) return false;
    const char *text = value.as<const char *>();
    if (text_equals(text, "EndCap")) {
        digital = kCodes[0];
        return true;
    }
    if (text_equals(text, "Internal")) {
        digital = kCodes[1];
        return true;
    }
    if (text_equals(text, "External")) {
        digital = kCodes[2];
        return true;
    }
    return false;
}

bool summary_digital_from_json_value(JsonVariantConst value,
                                     const char *rpc_name,
                                     int16_t &digital) {
    if (strcmp(rpc_name, "_ZHT") == 0) {
        return summary_tube_connected_code(value, digital);
    }
    if (strcmp(rpc_name, "_HUC") == 0) {
        return summary_humidifier_connected_code(value, digital);
    }
    return false;
}

}  // namespace

std::string edf_str_setting_get_names() {
    std::string names;
    names.reserve(512);
    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::SettingGet ||
            !signal->short_tag) {
            continue;
        }

        (void)append_str_get_name(names, signal->short_tag);
    }
    return names;
}

std::string edf_str_summary_get_names() {
    std::string names;
    names.reserve(512);
    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::Summary ||
            !signal->short_tag) {
            continue;
        }

        (void)append_str_get_name(names, signal->short_tag);
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

    JsonObjectConst json_result = get_value_object(doc);
    if (json_result.isNull()) {
        result.error = json_member_present(payload, "error")
                           ? "str_settings_rpc_error"
                           : "str_settings_missing_result";
        return false;
    }

    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::SettingGet ||
            !signal->short_tag) {
            continue;
        }

        char rpc_name[8] = {};
        rpc_name_for_str_tag(signal->short_tag, rpc_name, sizeof(rpc_name));
        JsonVariantConst value = json_result[rpc_name];
        if (value.isNull()) {
            result.missing++;
            continue;
        }

        int16_t digital = 0;
        if (str_text_digital_from_json_value(value, rpc_name, digital)) {
            if (session.set_signal_digital(i, digital)) {
                result.values++;
            } else {
                result.unmapped++;
            }
            continue;
        }
        if (str_text_digital_map_is_exclusive(rpc_name)) {
            result.unmapped++;
            continue;
        }

        const EdfStrDigitalRemap *remap =
            str_digital_remap_for_rpc_name(rpc_name);
        if (remap) {
            if (str_digital_from_json_value(value, rpc_name, *remap, digital) &&
                session.set_signal_digital(i, digital)) {
                result.values++;
            } else {
                result.unmapped++;
            }
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

bool edf_str_apply_summary_get_response(const std::string &payload,
                                        EdfStrSessionAccumulator &session,
                                        EdfStrSettingsApplyResult &result) {
    result = {};

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        result.error = "str_summary_json_failed";
        return false;
    }

    JsonObjectConst json_result = get_value_object(doc);
    if (json_result.isNull()) {
        result.error = json_member_present(payload, "error")
                           ? "str_summary_rpc_error"
                           : "str_summary_missing_result";
        return false;
    }

    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::Summary ||
            !signal->short_tag) {
            continue;
        }

        char rpc_name[8] = {};
        rpc_name_for_str_tag(signal->short_tag, rpc_name, sizeof(rpc_name));
        JsonVariantConst value = json_result[rpc_name];
        if (value.isNull()) {
            result.missing++;
            continue;
        }

        int16_t digital = 0;
        if (summary_digital_from_json_value(value, rpc_name, digital)) {
            if (session.set_signal_digital(i, digital)) {
                result.values++;
            } else {
                result.unmapped++;
            }
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
