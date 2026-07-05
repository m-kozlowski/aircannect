#include "as11_settings.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "as11_rpc.h"
#ifdef ARDUINO
#include "memory_manager.h"
#endif
#include "string_util.h"

namespace aircannect {
namespace {

void *settings_alloc_large(size_t size) {
#ifdef ARDUINO
    return Memory::alloc_large(size);
#else
    return malloc(size);
#endif
}

void settings_free(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

#define MODE_BIT(mode) (static_cast<uint16_t>(1u << (mode)))
#define MODES_NONE 0x0000u
#define MODES_ALL 0x07FFu
#define MODES_CPAP MODE_BIT(0)
#define MODES_AUTO (MODE_BIT(1) | MODE_BIT(2))
#define MODES_BILEVEL (MODE_BIT(3) | MODE_BIT(4) | MODE_BIT(5) | \
                       MODE_BIT(6) | MODE_BIT(9) | MODE_BIT(10))
#define MODES_VAUTO MODE_BIT(6)
#define MODES_ASV MODE_BIT(7)
#define MODES_ASVAUTO MODE_BIT(8)
#define MODES_IVAPS MODE_BIT(9)

template <typename T, size_t N>
constexpr uint8_t option_count(const T (&)[N]) {
    return static_cast<uint8_t>(N);
}

#include "as11_settings_catalog.inc"

constexpr size_t SETTINGS_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
constexpr size_t SETTING_COMPOSITES_COUNT =
    sizeof(SETTING_COMPOSITES) / sizeof(SETTING_COMPOSITES[0]);
static_assert(SETTINGS_COUNT <= As11SettingsState::MaxSettings,
              "As11SettingsState value storage too small");

bool json_is_number(JsonVariantConst value) {
    return value.is<int>() || value.is<unsigned int>() ||
           value.is<long>() || value.is<unsigned long>() ||
           value.is<long long>() || value.is<unsigned long long>() ||
           value.is<float>() || value.is<double>();
}

std::string value_to_string(JsonVariantConst value) {
    if (value.is<const char *>()) return value.as<const char *>();
    if (value.is<bool>()) return value.as<bool>() ? "true" : "false";
    if (value.is<int>()) return std::to_string(value.as<int>());
    if (value.is<unsigned int>()) return std::to_string(value.as<unsigned int>());
    if (value.is<long>()) return std::to_string(value.as<long>());
    if (value.is<unsigned long>()) return std::to_string(value.as<unsigned long>());
    if (value.is<long long>()) return std::to_string(value.as<long long>());
    if (value.is<unsigned long long>()) {
        return std::to_string(value.as<unsigned long long>());
    }
    if (value.is<float>() || value.is<double>()) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.3f", value.as<double>());
        char *end = buf + strlen(buf);
        while (end > buf && end[-1] == '0') *--end = 0;
        if (end > buf && end[-1] == '.') *--end = 0;
        return buf;
    }
    return "";
}

bool parse_number(const std::string &text, double &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    value = strtod(text.c_str(), &end);
    return end && *end == 0;
}

bool parse_iso_seconds(const char *text, double &seconds) {
    if (!text || strncmp(text, "PT", 2) != 0) return false;
    const char *start = text + 2;
    char *end = nullptr;
    seconds = strtod(start, &end);
    return end && end != start && end[0] == 'S' && end[1] == 0;
}

std::string compact_mode_key(const char *value) {
    std::string out;
    if (!value) return out;
    while (*value) {
        const unsigned char c = static_cast<unsigned char>(*value++);
        if (c == ' ' || c == '_' || c == '-') continue;
        out.push_back(static_cast<char>(tolower(c)));
    }
    const char suffix[] = "profile";
    const size_t suffix_len = sizeof(suffix) - 1;
    if (out.size() > suffix_len &&
        out.compare(out.size() - suffix_len, suffix_len, suffix) == 0) {
        out.resize(out.size() - suffix_len);
    }
    if (out == "autosetforher") return "autosether";
    return out;
}

bool parse_int_text(const std::string &text, int &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    long parsed = strtol(text.c_str(), &end, 10);
    if (!end || *end != 0) return false;
    value = static_cast<int>(parsed);
    return true;
}

int option_index_of(const As11SettingDef &def, const char *value) {
    if (!value) return -1;
    for (uint8_t i = 0; i < def.option_count; ++i) {
        if (strcmp(def.options[i], value) == 0) return i;
    }
    if (def.wire_options) {
        for (uint8_t i = 0; i < def.option_count; ++i) {
            if (strcmp(def.wire_options[i], value) == 0) return i;
        }
    }
    return -1;
}

const char *option_wire_value_at(const As11SettingDef &def, int index) {
    if (index < 0 || index >= def.option_count) return nullptr;
    return def.wire_options ? def.wire_options[index] : def.options[index];
}

bool rpc_name_matches_key(const char *rpc_name, const char *key) {
    if (!rpc_name || !key) return false;
    if (*rpc_name == '_') rpc_name++;
    if (*key == '_') key++;
    return strcmp(rpc_name, key) == 0;
}

bool setting_is_therapy_mode(const As11SettingDef &def) {
    return rpc_name_matches_key(def.key, "MOP");
}

const char *setting_field_name(const As11SettingDef &def) {
    return def.source_field ? def.source_field : def.key;
}

const As11SettingDef *setting_def_for_rpc_name(const char *rpc_name) {
    if (!rpc_name) return nullptr;
    for (const As11SettingDef &def : SETTINGS) {
        if (rpc_name_matches_key(rpc_name, def.key)) {
            return &def;
        }
    }
    return nullptr;
}

const char *profile_name_for_mode(int mode) {
    static const char *const names[] = {
        "CpapProfile",
        "AutoSetProfile",
        "AutoSetForHerProfile",
        "SpontProfile",
        "STProfile",
        "TimedProfile",
        "VAutoProfile",
        "ASVProfile",
        "ASVAutoProfile",
        "iVAPSProfile",
        "PACProfile",
    };
    if (mode < 0 || mode >= static_cast<int>(sizeof(names) / sizeof(names[0]))) {
        return nullptr;
    }
    return names[mode];
}

const char *profile_long_name_prefix(As11ProfileId profile) {
    switch (profile) {
        case As11ProfileId::Cpap:
            return "Cpap";
        case As11ProfileId::AutoSet:
            return "AutoSet";
        case As11ProfileId::HerAuto:
            return "HerAuto";
        case As11ProfileId::Spont:
            return "Spont";
        case As11ProfileId::ST:
            return "ST";
        case As11ProfileId::Timed:
            return "Timed";
        case As11ProfileId::VAuto:
            return "VAuto";
        case As11ProfileId::ASV:
            return "ASV";
        case As11ProfileId::ASVAuto:
            return "ASVAuto";
        case As11ProfileId::iVAPS:
            return "iVAPS";
        case As11ProfileId::PAC:
            return "PAC";
        case As11ProfileId::None:
            return nullptr;
    }

    return nullptr;
}

}  // namespace

bool as11_setting_option_index_for_rpc_name(const char *rpc_name,
                                            const char *wire_value,
                                            int16_t &index) {
    if (!rpc_name || !wire_value) return false;

    const As11SettingDef *def = setting_def_for_rpc_name(rpc_name);
    if (!def) return false;

    if (setting_is_therapy_mode(*def)) {
        for (int mode = 0; mode <= 10; ++mode) {
            const char *profile = profile_name_for_mode(mode);
            if (profile && strcmp(profile, wire_value) == 0) {
                index = static_cast<int16_t>(mode);
                return true;
            }
            if (mode < def->option_count &&
                strcmp(def->options[mode], wire_value) == 0) {
                index = static_cast<int16_t>(mode);
                return true;
            }
        }
        return false;
    }

    if (!def->options || !def->option_count) return false;
    const int matched_index = option_index_of(*def, wire_value);
    if (matched_index < 0) return false;

    index = static_cast<int16_t>(matched_index);
    return true;
}

namespace {

uint16_t profile_modes_from_result(JsonObjectConst result) {
    uint16_t mask = 0;
    JsonObjectConst profiles =
        result["TherapyProfiles"].as<JsonObjectConst>();
    if (profiles.isNull()) return 0;
    for (int mode = 0; mode <= 10; ++mode) {
        const char *profile_name = profile_name_for_mode(mode);
        if (!profile_name) continue;
        JsonObjectConst profile =
            profiles[profile_name].as<JsonObjectConst>();
        if (!profile.isNull()) {
            mask |= MODE_BIT(mode);
        }
    }
    return mask;
}

const char *profile_object_name(As11ProfileId profile) {
    switch (profile) {
        case As11ProfileId::Cpap:
            return "CpapProfile";
        case As11ProfileId::AutoSet:
            return "AutoSetProfile";
        case As11ProfileId::HerAuto:
            return "AutoSetForHerProfile";
        case As11ProfileId::Spont:
            return "SpontProfile";
        case As11ProfileId::ST:
            return "STProfile";
        case As11ProfileId::Timed:
            return "TimedProfile";
        case As11ProfileId::VAuto:
            return "VAutoProfile";
        case As11ProfileId::ASV:
            return "ASVProfile";
        case As11ProfileId::ASVAuto:
            return "ASVAutoProfile";
        case As11ProfileId::iVAPS:
            return "iVAPSProfile";
        case As11ProfileId::PAC:
            return "PACProfile";
        case As11ProfileId::None:
            return nullptr;
    }

    return nullptr;
}

int profile_mode_index(As11ProfileId profile) {
    if (profile == As11ProfileId::None) return -1;

    const uint8_t raw = static_cast<uint8_t>(profile);
    return raw <= static_cast<uint8_t>(As11ProfileId::PAC) ? raw : -1;
}

JsonObjectConst therapy_profile_object_from_result(
    JsonObjectConst result,
    const As11SettingDef &def) {
    if (def.source != As11SettingSource::TherapyProfile) {
        return JsonObjectConst();
    }

    const char *profile_name = profile_object_name(def.profile);
    if (!profile_name) return JsonObjectConst();

    JsonObjectConst profile = result[profile_name].as<JsonObjectConst>();
    if (!profile.isNull()) return profile;

    return result["TherapyProfiles"][profile_name].as<JsonObjectConst>();
}

const char *setting_rpc_key(const As11SettingDef &def,
                            char *buffer,
                            size_t buffer_len) {
    if (!def.key || buffer_len < 2) return nullptr;

    const int written = snprintf(buffer, buffer_len, "_%s", def.key);
    if (written < 0 || static_cast<size_t>(written) >= buffer_len) {
        return nullptr;
    }

    return buffer;
}

JsonVariantConst value_for_setting(JsonObjectConst object,
                                   const As11SettingDef &def) {
    char key[8];
    const char *rpc_key = setting_rpc_key(def, key, sizeof(key));
    return rpc_key ? object[rpc_key] : JsonVariantConst();
}

JsonObjectConst nested_object(JsonObjectConst root, const char *path) {
    JsonObjectConst current = root;
    if (!path || current.isNull()) return JsonObjectConst();

    const char *segment = path;
    while (*segment) {
        const char *dot = strchr(segment, '.');
        if (!dot) return current[segment].as<JsonObjectConst>();

        char key[48];
        const size_t len = static_cast<size_t>(dot - segment);
        if (len == 0 || len >= sizeof(key)) return JsonObjectConst();
        memcpy(key, segment, len);
        key[len] = 0;

        current = current[key].as<JsonObjectConst>();
        if (current.isNull()) return JsonObjectConst();
        segment = dot + 1;
    }
    return current;
}

JsonObjectConst feature_object_from_result(JsonObjectConst result,
                                           const As11SettingDef &def) {
    if (def.source != As11SettingSource::FeatureProfile) {
        return JsonObjectConst();
    }

    JsonObjectConst profiles = result["FeatureProfiles"].as<JsonObjectConst>();
    return nested_object(profiles, def.source_object);
}

int enum_index_from_text(const As11SettingDef &def, const char *text) {
    int index = option_index_of(def, text);
    if (index >= 0) return index;

    int parsed = -1;
    if (text && parse_int_text(text, parsed)) return parsed;

    if (!setting_is_therapy_mode(def)) return -1;
    const std::string wanted = compact_mode_key(text);
    if (wanted.empty()) return -1;
    for (uint8_t i = 0; i < def.option_count; ++i) {
        if (compact_mode_key(def.options[i]) == wanted) return i;
    }
    return -1;
}

int mode_index_from_json(JsonVariantConst value) {
    if (value.isNull()) return -1;
    if (value.is<int>()) return value.as<int>();
    if (value.is<long>()) return static_cast<int>(value.as<long>());
    if (value.is<const char *>()) {
        return as11_mode_index_from_value(value.as<const char *>());
    }
    if (value.is<std::string>()) {
        return as11_mode_index_from_value(value.as<std::string>());
    }
    return as11_mode_index_from_value(value_to_string(value));
}

bool setting_uses_iso_seconds(const As11SettingDef &def) {
    const char *field = setting_field_name(def);
    if (!field) return false;
    return strstr(field, "InspiratoryTime") != nullptr ||
           strcmp(field, "RiseTime") == 0 ||
           strcmp(field, "FallTime") == 0;
}

std::string normalize_value_for_def(const As11SettingDef &def,
                                    JsonVariantConst value) {
    if (def.kind == As11SettingKind::Number && def.scale_div > 1) {
        double numeric = 0;
        bool parsed = false;
        if (value.is<const char *>()) {
            parsed = parse_iso_seconds(value.as<const char *>(), numeric);
            if (!parsed) parsed = parse_number(value.as<const char *>(), numeric);
        } else if (json_is_number(value)) {
            numeric = value.as<double>();
            parsed = true;
        }
        if (parsed) {
            return std::to_string(lround(numeric * def.scale_div));
        }
    }

    if (def.kind == As11SettingKind::Enum) {
        int index = -1;
        if (value.is<int>()) {
            index = value.as<int>();
        } else if (value.is<long>()) {
            index = static_cast<int>(value.as<long>());
        } else if (value.is<bool>()) {
            index = value.as<bool>() ? 1 : 0;
        } else if (value.is<const char *>()) {
            index = enum_index_from_text(def, value.as<const char *>());
        }
        if (index >= 0 && index < def.option_count) {
            return std::to_string(index);
        }
    }
    return value_to_string(value);
}

bool setting_value_matches(const As11SettingDef &def,
                           const std::string &confirmed,
                           const std::string &pending) {
    if (def.kind == As11SettingKind::Number) {
        double a = 0;
        double b = 0;
        if (!parse_number(confirmed, a) || !parse_number(pending, b)) {
            return confirmed == pending;
        }
        const double tolerance = def.step > 0 ? def.step / 20.0 : 0.001;
        return fabs(a - b) <= tolerance;
    }

    if (def.kind == As11SettingKind::Bool) {
        bool a = false;
        bool b = false;
        if (parse_bool_yesno(confirmed, a) && parse_bool_yesno(pending, b)) {
            return a == b;
        }
    }

    return confirmed == pending;
}

bool json_literal_for_set(const As11SettingDef &def,
                          JsonVariantConst value,
                          std::string &out) {
    if (setting_is_therapy_mode(def)) {
        int index = mode_index_from_json(value);
        const char *profile = profile_name_for_mode(index);
        if (!profile) return false;
        out = "\"";
        out += profile;
        out += "\"";
        return true;
    }

    if (def.kind == As11SettingKind::Number) {
        if (!json_is_number(value)) return false;
        const double numeric = value.as<double>();
        if (setting_uses_iso_seconds(def)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "\"PT%gS\"", numeric);
            out = buf;
        } else {
            out = value_to_string(value);
        }
        return !out.empty();
    }

    if (def.kind == As11SettingKind::Enum) {
        int index = -1;
        if (value.is<int>()) {
            index = value.as<int>();
        } else if (value.is<long>()) {
            index = static_cast<int>(value.as<long>());
        } else if (value.is<bool>()) {
            index = value.as<bool>() ? 1 : 0;
        } else if (value.is<const char *>()) {
            index = enum_index_from_text(def, value.as<const char *>());
        }
        const char *wire_value = option_wire_value_at(def, index);
        if (!wire_value) return false;
        out = "\"";
        out += json_escape(wire_value);
        out += "\"";
        return true;
    }

    if (value.is<bool>()) {
        if (def.kind != As11SettingKind::Bool) return false;
        out = value.as<bool>() ? "true" : "false";
        return true;
    }

    if (def.kind == As11SettingKind::Text && value.is<const char *>()) {
        out = "\"";
        out += json_escape(value.as<const char *>());
        out += "\"";
        return true;
    }
    return false;
}

}  // namespace

As11StoredValue::As11StoredValue(const As11StoredValue &other) {
    set(other.str());
}

As11StoredValue::As11StoredValue(As11StoredValue &&other) noexcept {
    if (other.heap_) {
        heap_ = other.heap_;
        length_ = other.length_;
        other.heap_ = nullptr;
        other.length_ = 0;
        other.inline_[0] = 0;
        return;
    }
    if (other.length_) {
        memcpy(inline_, other.inline_, other.length_ + 1);
        length_ = other.length_;
        other.clear();
    }
}

As11StoredValue::~As11StoredValue() {
    clear();
}

As11StoredValue &As11StoredValue::operator=(const As11StoredValue &other) {
    if (this != &other) set(other.str());
    return *this;
}

As11StoredValue &As11StoredValue::operator=(
    As11StoredValue &&other) noexcept {
    if (this == &other) return *this;
    clear();
    if (other.heap_) {
        heap_ = other.heap_;
        length_ = other.length_;
        other.heap_ = nullptr;
        other.length_ = 0;
        other.inline_[0] = 0;
        return *this;
    }
    if (other.length_) {
        memcpy(inline_, other.inline_, other.length_ + 1);
        length_ = other.length_;
        other.clear();
    }
    return *this;
}

bool As11StoredValue::set(const std::string &value) {
    const size_t len = value.size();
    if (len <= InlineCapacity) {
        settings_free(heap_);
        heap_ = nullptr;
        length_ = len;
        if (len) memcpy(inline_, value.data(), len);
        inline_[len] = 0;
        return true;
    }

    char *next = static_cast<char *>(settings_alloc_large(len + 1));
    if (!next) return false;
    memcpy(next, value.data(), len);
    next[len] = 0;
    settings_free(heap_);
    heap_ = next;
    length_ = len;
    inline_[0] = 0;
    return true;
}

void As11StoredValue::clear() {
    settings_free(heap_);
    heap_ = nullptr;
    length_ = 0;
    inline_[0] = 0;
}

bool As11SettingsState::apply_settings_get_response(
    const std::string &payload,
    uint32_t now_ms) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) {
        result = doc["error"]["data"].as<JsonObjectConst>();
    }
    if (result.isNull()) return false;

    int fallback_mode = mode_index();
    JsonVariantConst active = result["_MOP"];
    if (!active.isNull()) {
        fallback_mode = as11_mode_index_from_value(value_to_string(active));
    }
    const uint16_t profile_modes = profile_modes_from_result(result);
    if (profile_modes) {
        supported_mode_mask_ = profile_modes;
        clear_profile_values();
    }

    bool any = false;
    auto remember_value = [&](size_t index, const std::string &normalized) {
        values_[index] = normalized;
        if (pending_[index] &&
            setting_value_matches(SETTINGS[index],
                                  values_[index],
                                  pending_values_[index])) {
            clear_pending(index);
            last_write_status_ = pending_count_ ? "waiting_readback"
                                                : "confirmed";
            last_write_ms_ = now_ms;
        }
        any = true;
    };
    auto remember_profile_value = [&](int mode,
                                      size_t index,
                                      const std::string &normalized) {
        set_profile_value(mode, index, normalized);
        if (mode == fallback_mode) values_[index] = normalized;
        if (pending_[index] &&
            setting_value_matches(SETTINGS[index],
                                  normalized,
                                  pending_values_[index])) {
            clear_pending(index);
            last_write_status_ = pending_count_ ? "waiting_readback"
                                                : "confirmed";
            last_write_ms_ = now_ms;
        }
        any = true;
    };

    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        JsonVariantConst value;
        if (SETTINGS[i].source == As11SettingSource::FeatureProfile) {
            JsonObjectConst feature = feature_object_from_result(result,
                                                                 SETTINGS[i]);
            if (feature.isNull()) continue;
            feature_present_[i] = true;
            value = feature[SETTINGS[i].source_field];
            if (value.isNull()) {
                values_[i] = "";
                continue;
            }
        } else {
            value = value_for_setting(result, SETTINGS[i]);
            if (value.isNull()) continue;
        }
        remember_value(i, normalize_value_for_def(SETTINGS[i], value));
    }

    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (SETTINGS[i].source != As11SettingSource::TherapyProfile) {
            continue;
        }

        JsonObjectConst profile =
            therapy_profile_object_from_result(result, SETTINGS[i]);
        if (profile.isNull()) continue;

        JsonVariantConst value = profile[SETTINGS[i].source_field];
        if (value.isNull()) continue;

        const int mode = profile_mode_index(SETTINGS[i].profile);
        if (!as11_setting_visible_for_mode(SETTINGS[i], mode)) continue;

        remember_profile_value(
            mode, i, normalize_value_for_def(SETTINGS[i], value));
    }

    if (any) {
        valid_ = true;
        updated_ms_ = now_ms;
    }
    return any;
}

bool As11SettingsState::note_set_request(const std::string &params_json,
                                         uint32_t now_ms) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, params_json);
    if (err || !doc.is<JsonObjectConst>()) return false;

    JsonObjectConst root = doc.as<JsonObjectConst>();
    int mode = mode_index();
    int target_mode = mode_index_from_json(root["MOP"]);
    if (target_mode < 0) {
        target_mode = mode_index_from_json(root["_MOP"]);
    }
    if (target_mode >= 0) mode = target_mode;

    bool any = false;
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (!as11_setting_visible_for_mode(SETTINGS[i], mode)) continue;
        JsonVariantConst value = value_for_setting(root, SETTINGS[i]);
        if (value.isNull()) continue;
        std::string pending_value = normalize_value_for_def(SETTINGS[i], value);
        if (pending_value.empty()) continue;
        if (!pending_[i]) pending_count_++;
        pending_[i] = true;
        pending_values_[i] = pending_value;
        pending_since_ms_[i] = now_ms;
        any = true;
    }

    if (any) {
        last_write_status_ = "sent";
        last_write_ms_ = now_ms;
    }
    return any;
}

void As11SettingsState::note_set_response(bool is_error, uint32_t now_ms) {
    if (!pending_count_) return;
    if (is_error) {
        clear_all_pending();
        last_write_status_ = "set_error";
    } else {
        last_write_status_ = "waiting_readback";
    }
    last_write_ms_ = now_ms;
}

void As11SettingsState::note_set_cancelled(const char *reason,
                                           uint32_t now_ms) {
    if (!pending_count_) return;
    clear_all_pending();
    last_write_status_ = reason ? reason : "cancelled";
    last_write_ms_ = now_ms;
}

void As11SettingsState::clear() {
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        values_[i] = "";
        feature_present_[i] = false;
        pending_values_[i] = "";
        pending_since_ms_[i] = 0;
        pending_[i] = false;
    }
    clear_profile_values();
    pending_count_ = 0;
    last_write_status_ = "";
    last_write_ms_ = 0;
    valid_ = false;
    updated_ms_ = 0;
    supported_mode_mask_ = 0;
}

void As11SettingsState::clear_pending(size_t index) {
    if (index >= SETTINGS_COUNT || !pending_[index]) return;
    pending_[index] = false;
    pending_values_[index] = "";
    pending_since_ms_[index] = 0;
    if (pending_count_) pending_count_--;
}

void As11SettingsState::clear_all_pending() {
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) clear_pending(i);
}

void As11SettingsState::clear_profile_values() {
    for (ProfileValueSlot &slot : profile_values_) {
        slot.used = false;
        slot.mode = 0;
        slot.index = 0;
        slot.value.clear();
    }
}

const As11StoredValue *As11SettingsState::profile_value(
    size_t index,
    int mode) const {
    if (index >= SETTINGS_COUNT || mode < 0 ||
        mode >= static_cast<int>(MaxModes)) {
        return nullptr;
    }
    for (const ProfileValueSlot &slot : profile_values_) {
        if (!slot.used) continue;
        if (slot.mode == static_cast<uint8_t>(mode) &&
            slot.index == static_cast<uint8_t>(index)) {
            return &slot.value;
        }
    }
    return nullptr;
}

bool As11SettingsState::set_profile_value(int mode,
                                          size_t index,
                                          const std::string &value) {
    if (mode < 0 || mode >= static_cast<int>(MaxModes) ||
        index >= SETTINGS_COUNT) {
        return false;
    }

    for (ProfileValueSlot &slot : profile_values_) {
        if (!slot.used) continue;
        if (slot.mode == static_cast<uint8_t>(mode) &&
            slot.index == static_cast<uint8_t>(index)) {
            return slot.value.set(value);
        }
    }

    for (ProfileValueSlot &slot : profile_values_) {
        if (slot.used) continue;
        slot.mode = static_cast<uint8_t>(mode);
        slot.index = static_cast<uint8_t>(index);
        if (!slot.value.set(value)) {
            slot.mode = 0;
            slot.index = 0;
            return false;
        }
        slot.used = true;
        return true;
    }

    return false;
}

std::string As11SettingsState::value(size_t index, int mode) const {
    if (index >= SETTINGS_COUNT) return "";
    if (mode >= 0 && mode < static_cast<int>(MaxModes)) {
        if (setting_is_therapy_mode(SETTINGS[index])) {
            return std::to_string(mode);
        }
        const As11StoredValue *stored = profile_value(index, mode);
        if (SETTINGS[index].source == As11SettingSource::TherapyProfile &&
            stored && !stored->empty()) {
            return stored->str();
        }
    }
    return values_[index];
}

bool As11SettingsState::feature_present(size_t index) const {
    if (index >= SETTINGS_COUNT) return false;
    return feature_present_[index];
}

bool As11SettingsState::setting_visible(size_t index, int mode) const {
    if (index >= SETTINGS_COUNT) return false;
    const As11SettingDef &def = SETTINGS[index];
    if (!as11_setting_visible_for_mode(def, mode)) return false;
    if (def.source != As11SettingSource::FeatureProfile) return true;
    return feature_present_[index];
}

int As11SettingsState::mode_index() const {
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (setting_is_therapy_mode(SETTINGS[i])) {
            return as11_mode_index_from_value(values_[i]);
        }
    }
    return -1;
}

size_t as11_setting_count() {
    return SETTINGS_COUNT;
}

const As11SettingDef &as11_setting(size_t index) {
    return SETTINGS[index];
}

const As11SettingDef *as11_find_setting(const char *key) {
    if (!key) return nullptr;
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (rpc_name_matches_key(key, SETTINGS[i].key)) {
            return &SETTINGS[i];
        }
    }
    return nullptr;
}

std::string as11_setting_rpc_long_name(const As11SettingDef &def) {
    if (setting_is_therapy_mode(def)) return "ActiveTherapyProfile";

    const char *field = setting_field_name(def);
    if (!field || !field[0]) return def.key ? def.key : "";

    if (def.source == As11SettingSource::TherapyProfile) {
        const char *prefix = profile_long_name_prefix(def.profile);
        if (!prefix || !prefix[0]) return field;

        std::string out(prefix);
        out += "-";
        out += field;
        return out;
    }

    return field;
}

size_t as11_setting_composite_count() {
    return SETTING_COMPOSITES_COUNT;
}

const As11SettingCompositeDef &as11_setting_composite(size_t index) {
    return SETTING_COMPOSITES[index];
}

bool as11_setting_visible_for_mode(const As11SettingDef &def, int mode) {
    if (mode < 0 || mode > 10) return setting_is_therapy_mode(def);
    return (def.mode_mask & MODE_BIT(mode)) != 0;
}

bool as11_setting_option_supported(const As11SettingDef &def,
                                   uint8_t option_index,
                                   uint16_t supported_mode_mask) {
    if (!setting_is_therapy_mode(def)) return true;
    if (option_index >= def.option_count) return false;
    return supported_mode_mask &&
           (supported_mode_mask & (1u << option_index)) != 0;
}

bool as11_setting_readable_via_rpc(const As11SettingDef &def) {
    return def.source == As11SettingSource::Flat ||
           (def.source == As11SettingSource::TherapyProfile &&
            def.profile != As11ProfileId::None &&
            def.source_field != nullptr) ||
           (def.source == As11SettingSource::FeatureProfile &&
            def.source_object != nullptr && def.source_field != nullptr);
}

bool as11_setting_writable_via_rpc(const As11SettingDef &def) {
    char key[80];
    return setting_rpc_key(def, key, sizeof(key)) != nullptr;
}

int as11_mode_index_from_value(const std::string &value) {
    int parsed = -1;
    if (parse_int_text(value, parsed) && parsed >= 0 && parsed <= 10) {
        return parsed;
    }
    const As11SettingDef *mode_def = as11_find_setting("MOP");
    if (!mode_def) return -1;
    const int index = enum_index_from_text(*mode_def, value.c_str());
    return index >= 0 && index <= 10 ? index : -1;
}

const char *as11_mode_name(int mode) {
    static const char *const names[] = {
        "CPAP", "AutoSet", "AutoSet For Her", "S", "ST", "T",
        "VAuto", "ASV", "ASVAuto", "iVAPS", "PAC",
    };
    if (mode < 0 || mode >= static_cast<int>(sizeof(names) / sizeof(names[0]))) {
        return "";
    }
    return names[mode];
}

std::string as11_settings_get_params_json() {
    std::string out = "[";
    out += "\"_MOP\",\"TherapyProfiles\",\"FeatureProfiles\"";
    out += "]";
    return out;
}

std::string as11_build_set_params_from_json(const std::string &body,
                                            int mode,
                                            size_t &accepted) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc.is<JsonObjectConst>()) {
        accepted = 0;
        return "{}";
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();
    int target_mode = mode_index_from_json(root["MOP"]);
    if (target_mode >= 0) mode = target_mode;

    std::string out = "{";
    accepted = 0;
    for (size_t i = 0; i < SETTINGS_COUNT; ++i) {
        if (!as11_setting_visible_for_mode(SETTINGS[i], mode)) continue;
        JsonVariantConst value = root[SETTINGS[i].key];
        if (value.isNull()) continue;
        char key[80];
        const char *rpc_key = setting_rpc_key(SETTINGS[i],
                                              key,
                                              sizeof(key));
        if (!rpc_key) continue;
        std::string literal;
        if (!json_literal_for_set(SETTINGS[i], value, literal)) continue;
        if (accepted) out += ",";
        out += "\"";
        out += rpc_key;
        out += "\":";
        out += literal;
        accepted++;
    }
    out += "}";
    return out;
}

}  // namespace aircannect
