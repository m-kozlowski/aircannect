#include "as11_settings.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <utility>

#include <array>

#include "as11_rpc.h"
#include "as11_setting_catalog_builder.h"
#include "memory_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

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
#define MODES_PAC MODE_BIT(10)

struct TherapyProfileDescriptor {
    uint8_t mode_index;
    As11ProfileId profile;
    const char *display_name;
    const char *object_name;
    const char *rpc_prefix;
    const char *wire_name;
};

constexpr TherapyProfileDescriptor THERAPY_PROFILES[] = {
    {0, As11ProfileId::Cpap,
     "CPAP", "CpapProfile", "Cpap", "CpapProfile"},
    {1, As11ProfileId::AutoSet,
     "AutoSet", "AutoSetProfile", "AutoSet", "AutoSetProfile"},
    {2, As11ProfileId::HerAuto,
     "AutoSet For Her", "AutoSetForHerProfile", "HerAuto",
     "AutoSetForHerProfile"},
    {3, As11ProfileId::Spont,
     "S", "SpontProfile", "Spont", "SpontProfile"},
    {4, As11ProfileId::ST,
     "ST", "STProfile", "ST", "STProfile"},
    {5, As11ProfileId::Timed,
     "T", "TimedProfile", "Timed", "TimedProfile"},
    {6, As11ProfileId::VAuto,
     "VAuto", "VAutoProfile", "VAuto", "VAutoProfile"},
    {7, As11ProfileId::ASV,
     "ASV", "ASVProfile", "ASV", "ASVProfile"},
    {8, As11ProfileId::ASVAuto,
     "ASVAuto", "ASVAutoProfile", "ASVAuto", "ASVAutoProfile"},
    {9, As11ProfileId::iVAPS,
     "iVAPS", "iVAPSProfile", "iVAPS", "iVAPSProfile"},
    {10, As11ProfileId::PAC,
     "PAC", "PACProfile", "PAC", "PACProfile"},
};

constexpr size_t THERAPY_PROFILE_COUNT =
    sizeof(THERAPY_PROFILES) / sizeof(THERAPY_PROFILES[0]);
static_assert(THERAPY_PROFILE_COUNT == As11SettingsState::MaxModes,
              "therapy profile table must cover every mode");

constexpr bool therapy_profiles_are_indexed() {
    for (size_t i = 0; i < THERAPY_PROFILE_COUNT; ++i) {
        if (THERAPY_PROFILES[i].mode_index != i) return false;
    }
    return true;
}

static_assert(therapy_profiles_are_indexed(),
              "therapy profiles must follow mode index order");

const TherapyProfileDescriptor *profile_for_mode(int mode) {
    if (mode < 0 || mode >= static_cast<int>(THERAPY_PROFILE_COUNT)) {
        return nullptr;
    }

    const TherapyProfileDescriptor &profile = THERAPY_PROFILES[mode];
    return profile.mode_index == mode ? &profile : nullptr;
}

const TherapyProfileDescriptor *profile_for_id(As11ProfileId id) {
    for (const TherapyProfileDescriptor &profile : THERAPY_PROFILES) {
        if (profile.profile == id) return &profile;
    }
    return nullptr;
}

constexpr std::array<const char *, THERAPY_PROFILE_COUNT>
therapy_mode_display_options() {
    std::array<const char *, THERAPY_PROFILE_COUNT> options = {};
    for (size_t i = 0; i < THERAPY_PROFILE_COUNT; ++i) {
        options[i] = THERAPY_PROFILES[i].display_name;
    }
    return options;
}

constexpr std::array<const char *, THERAPY_PROFILE_COUNT>
therapy_mode_wire_options() {
    std::array<const char *, THERAPY_PROFILE_COUNT> options = {};
    for (size_t i = 0; i < THERAPY_PROFILE_COUNT; ++i) {
        options[i] = THERAPY_PROFILES[i].wire_name;
    }
    return options;
}

constexpr auto THERAPY_MODE_OPTIONS = therapy_mode_display_options();
constexpr auto THERAPY_MODE_WIRE_OPTIONS = therapy_mode_wire_options();

#include "as11_settings_catalog.inc"

constexpr size_t SETTINGS_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
constexpr size_t SETTING_COMPOSITES_COUNT =
    sizeof(SETTING_COMPOSITES) / sizeof(SETTING_COMPOSITES[0]);

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

const char *profile_wire_name_for_mode(int mode) {
    const TherapyProfileDescriptor *profile = profile_for_mode(mode);
    return profile ? profile->wire_name : nullptr;
}

const char *profile_long_name_prefix(As11ProfileId profile) {
    const TherapyProfileDescriptor *descriptor = profile_for_id(profile);
    return descriptor ? descriptor->rpc_prefix : nullptr;
}

}  // namespace

bool as11_setting_option_index_for_rpc_name(const char *rpc_name,
                                            const char *wire_value,
                                            int16_t &index) {
    if (!rpc_name || !wire_value) return false;

    const As11SettingDef *def = setting_def_for_rpc_name(rpc_name);
    if (!def) return false;

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
    for (const TherapyProfileDescriptor &descriptor : THERAPY_PROFILES) {
        JsonObjectConst profile =
            profiles[descriptor.object_name].as<JsonObjectConst>();
        if (!profile.isNull()) {
            mask |= MODE_BIT(descriptor.mode_index);
        }
    }
    return mask;
}

const char *profile_object_name(As11ProfileId profile) {
    const TherapyProfileDescriptor *descriptor = profile_for_id(profile);
    return descriptor ? descriptor->object_name : nullptr;
}

int profile_mode_index(As11ProfileId profile) {
    const TherapyProfileDescriptor *descriptor = profile_for_id(profile);
    return descriptor ? descriptor->mode_index : -1;
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
    if (def.kind == As11SettingKind::Number &&
        (def.scale_div > 1 || setting_uses_iso_seconds(def))) {
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
        const char *profile = profile_wire_name_for_mode(index);
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
        Memory::free(heap_);
        heap_ = nullptr;
        length_ = len;
        if (len) memcpy(inline_, value.data(), len);
        inline_[len] = 0;
        return true;
    }

    char *next = static_cast<char *>(Memory::alloc_large(len + 1));
    if (!next) return false;
    memcpy(next, value.data(), len);
    next[len] = 0;
    Memory::free(heap_);
    heap_ = next;
    length_ = len;
    inline_[0] = 0;
    return true;
}

void As11StoredValue::clear() {
    Memory::free(heap_);
    heap_ = nullptr;
    length_ = 0;
    inline_[0] = 0;
}

As11SettingsState::~As11SettingsState() {
    release_storage();
}

bool As11SettingsState::ensure_storage() {
    const size_t catalog_count = catalog_.count();
    if (setting_capacity_ == catalog_count &&
        profile_capacity_ == MaxProfileValues &&
        values_ && pending_values_ && profile_values_ &&
        feature_present_ && pending_ && pending_since_ms_) {
        return true;
    }

    release_storage();

    values_ = static_cast<As11StoredValue *>(
        Memory::alloc_large(sizeof(As11StoredValue) * catalog_count));
    if (!values_) goto fail;
    setting_capacity_ = catalog_count;
    for (size_t i = 0; i < setting_capacity_; ++i) {
        new (&values_[i]) As11StoredValue();
    }

    pending_values_ = static_cast<As11StoredValue *>(
        Memory::alloc_large(sizeof(As11StoredValue) * setting_capacity_));
    if (!pending_values_) goto fail;
    for (size_t i = 0; i < setting_capacity_; ++i) {
        new (&pending_values_[i]) As11StoredValue();
    }

    profile_values_ = static_cast<ProfileValueSlot *>(
        Memory::alloc_large(sizeof(ProfileValueSlot) * MaxProfileValues));
    if (!profile_values_) goto fail;
    profile_capacity_ = MaxProfileValues;
    for (size_t i = 0; i < profile_capacity_; ++i) {
        new (&profile_values_[i]) ProfileValueSlot();
    }

    feature_present_ = static_cast<bool *>(
        Memory::alloc_large(sizeof(bool) * setting_capacity_));
    pending_ = static_cast<bool *>(
        Memory::alloc_large(sizeof(bool) * setting_capacity_));
    pending_since_ms_ = static_cast<uint32_t *>(
        Memory::alloc_large(sizeof(uint32_t) * setting_capacity_));
    if (!feature_present_ || !pending_ || !pending_since_ms_) goto fail;

    memset(feature_present_, 0, sizeof(bool) * setting_capacity_);
    memset(pending_, 0, sizeof(bool) * setting_capacity_);
    memset(pending_since_ms_, 0, sizeof(uint32_t) * setting_capacity_);
    return true;

fail:
    release_storage();
    last_write_status_ = "settings_alloc_failed";
    return false;
}

void As11SettingsState::release_storage() {
    if (values_) {
        for (size_t i = 0; i < setting_capacity_; ++i) {
            values_[i].~As11StoredValue();
        }
        Memory::free(values_);
    }

    if (pending_values_) {
        for (size_t i = 0; i < setting_capacity_; ++i) {
            pending_values_[i].~As11StoredValue();
        }
        Memory::free(pending_values_);
    }

    if (profile_values_) {
        for (size_t i = 0; i < profile_capacity_; ++i) {
            profile_values_[i].~ProfileValueSlot();
        }
        Memory::free(profile_values_);
    }

    Memory::free(feature_present_);
    Memory::free(pending_);
    Memory::free(pending_since_ms_);

    values_ = nullptr;
    pending_values_ = nullptr;
    profile_values_ = nullptr;
    feature_present_ = nullptr;
    pending_ = nullptr;
    pending_since_ms_ = nullptr;
    setting_capacity_ = 0;
    profile_capacity_ = 0;
}

bool As11SettingsState::apply_settings_get_response(
    RpcPayloadView payload,
    uint32_t now_ms,
    bool *complete_snapshot_out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, payload.data() ? payload.data() : "", payload.size());
    if (err) return false;

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) {
        result = doc["error"]["data"].as<JsonObjectConst>();
    }
    if (result.isNull()) return false;

    bool catalog_changed = false;
    if (!catalog_.apply_airbreak_info(
            result["AirbreakInfo"].as<JsonObjectConst>(), catalog_changed)) {
        return false;
    }
    if (catalog_changed) {
        release_storage();
        pending_count_ = 0;
        last_write_status_.clear();
        last_write_ms_ = 0;
        updated_ms_ = 0;
        supported_mode_mask_ = 0;
        valid_ = false;
    }
    if (!ensure_storage()) return false;

    const bool complete_snapshot =
        !result["TherapyProfiles"].as<JsonObjectConst>().isNull() &&
        !result["FeatureProfiles"].as<JsonObjectConst>().isNull();
    if (complete_snapshot_out) *complete_snapshot_out = complete_snapshot;

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
    bool pending_confirmed = false;
    bool pending_mismatched = false;
    bool storage_ok = true;
    auto remember_value = [&](size_t index, const std::string &normalized) {
        if (!values_[index].set(normalized)) {
            storage_ok = false;
            return;
        }
        if (pending_[index]) {
            const bool matches = setting_value_matches(
                catalog_.setting(index), values_[index].str(),
                pending_values_[index].str());
            if (matches || complete_snapshot) {
                clear_pending(index);
                pending_confirmed = pending_confirmed || matches;
                pending_mismatched = pending_mismatched || !matches;
            }
        }
        any = true;
    };
    auto remember_profile_value = [&](int mode,
                                      size_t index,
                                      const std::string &normalized) {
        if (!set_profile_value(mode, index, normalized)) {
            storage_ok = false;
            return;
        }
        if (mode == fallback_mode && !values_[index].set(normalized)) {
            storage_ok = false;
            return;
        }
        if (pending_[index]) {
            const bool matches = setting_value_matches(
                catalog_.setting(index), normalized,
                pending_values_[index].str());
            if (matches || complete_snapshot) {
                clear_pending(index);
                pending_confirmed = pending_confirmed || matches;
                pending_mismatched = pending_mismatched || !matches;
            }
        }
        any = true;
    };

    for (size_t i = 0; i < setting_capacity_; ++i) {
        const As11SettingDef &def = catalog_.setting(i);
        JsonVariantConst value;
        if (def.source == As11SettingSource::FeatureProfile) {
            JsonObjectConst feature = feature_object_from_result(result, def);
            if (feature.isNull()) continue;
            feature_present_[i] = true;
            value = feature[def.source_field];
            if (value.isNull()) {
                values_[i].clear();
                continue;
            }
        } else {
            value = value_for_setting(result, def);
            if (value.isNull()) continue;
        }
        remember_value(i, normalize_value_for_def(def, value));
    }

    for (size_t i = 0; i < setting_capacity_; ++i) {
        const As11SettingDef &def = catalog_.setting(i);
        if (def.source != As11SettingSource::TherapyProfile) {
            continue;
        }

        JsonObjectConst profile =
            therapy_profile_object_from_result(result, def);
        if (profile.isNull()) continue;

        JsonVariantConst value = profile[def.source_field];
        if (value.isNull()) continue;

        const int mode = profile_mode_index(def.profile);
        if (!as11_setting_visible_for_mode(def, mode)) continue;

        remember_profile_value(
            mode, i, normalize_value_for_def(def, value));
    }

    if (any && storage_ok && complete_snapshot) {
        valid_ = true;
        updated_ms_ = now_ms;
    }

    if (pending_mismatched) {
        last_write_status_ = "readback_mismatch";
        last_write_ms_ = now_ms;
    } else if (pending_confirmed) {
        last_write_status_ = pending_count_ ? "waiting_readback" : "confirmed";
        last_write_ms_ = now_ms;
    }

    return any && storage_ok;
}

bool As11SettingsState::note_set_request(const std::string &params_json,
                                         uint32_t now_ms) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, params_json);
    if (err || !doc.is<JsonObjectConst>()) return false;
    if (!ensure_storage()) return false;

    JsonObjectConst root = doc.as<JsonObjectConst>();
    int mode = mode_index();
    int target_mode = mode_index_from_json(root["MOP"]);
    if (target_mode < 0) {
        target_mode = mode_index_from_json(root["_MOP"]);
    }
    if (target_mode >= 0) mode = target_mode;

    bool any = false;
    for (size_t i = 0; i < setting_capacity_; ++i) {
        const As11SettingDef &def = catalog_.setting(i);
        if (!as11_setting_visible_for_mode(def, mode)) continue;
        JsonVariantConst value = value_for_setting(root, def);
        if (value.isNull()) continue;
        std::string pending_value = normalize_value_for_def(def, value);
        if (pending_value.empty()) continue;

        const bool was_pending = pending_[i];
        if (!pending_values_[i].set(pending_value)) {
            continue;
        }

        if (!was_pending) pending_count_++;
        pending_[i] = true;
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
        for (size_t i = 0; i < setting_capacity_; ++i) {
            if (pending_[i]) pending_since_ms_[i] = now_ms;
        }
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

bool As11SettingsState::expire_pending(uint32_t now_ms,
                                       uint32_t timeout_ms) {
    if (!pending_count_ || timeout_ms == 0) return false;

    bool expired = false;
    for (size_t i = 0; i < setting_capacity_; ++i) {
        if (!pending_[i]) continue;
        if (static_cast<uint32_t>(now_ms - pending_since_ms_[i]) < timeout_ms) {
            continue;
        }

        clear_pending(i);
        expired = true;
    }

    if (expired) {
        last_write_status_ = "readback_timeout";
        last_write_ms_ = now_ms;
    }
    return expired;
}

void As11SettingsState::clear() {
    if (!values_) {
        pending_count_ = 0;
        last_write_status_ = "";
        last_write_ms_ = 0;
        valid_ = false;
        updated_ms_ = 0;
        supported_mode_mask_ = 0;
        return;
    }

    for (size_t i = 0; i < setting_capacity_; ++i) {
        values_[i].clear();
        feature_present_[i] = false;
        pending_values_[i].clear();
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
    if (!pending_ || index >= setting_capacity_ || !pending_[index]) return;
    pending_[index] = false;
    if (pending_values_) pending_values_[index].clear();
    pending_since_ms_[index] = 0;
    if (pending_count_) pending_count_--;
}

void As11SettingsState::clear_all_pending() {
    for (size_t i = 0; i < setting_capacity_; ++i) clear_pending(i);
}

void As11SettingsState::clear_profile_values() {
    if (!profile_values_) return;

    for (size_t i = 0; i < profile_capacity_; ++i) {
        ProfileValueSlot &slot = profile_values_[i];
        slot.used = false;
        slot.mode = 0;
        slot.index = 0;
        slot.value.clear();
    }
}

const As11StoredValue *As11SettingsState::profile_value(
    size_t index,
    int mode) const {
    if (!profile_values_ || index >= setting_capacity_ || mode < 0 ||
        mode >= static_cast<int>(MaxModes)) {
        return nullptr;
    }
    for (size_t i = 0; i < profile_capacity_; ++i) {
        const ProfileValueSlot &slot = profile_values_[i];
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
    if (!ensure_storage() ||
        mode < 0 || mode >= static_cast<int>(MaxModes) ||
        index >= setting_capacity_) {
        return false;
    }

    for (size_t i = 0; i < profile_capacity_; ++i) {
        ProfileValueSlot &slot = profile_values_[i];
        if (!slot.used) continue;
        if (slot.mode == static_cast<uint8_t>(mode) &&
            slot.index == static_cast<uint8_t>(index)) {
            return slot.value.set(value);
        }
    }

    for (size_t i = 0; i < profile_capacity_; ++i) {
        ProfileValueSlot &slot = profile_values_[i];
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
    if (!values_ || index >= setting_capacity_) return "";
    const As11SettingDef &def = catalog_.setting(index);
    if (mode >= 0 && mode < static_cast<int>(MaxModes)) {
        if (setting_is_therapy_mode(def)) {
            return std::to_string(mode);
        }
        const As11StoredValue *stored = profile_value(index, mode);
        if (def.source == As11SettingSource::TherapyProfile &&
            stored && !stored->empty()) {
            return stored->str();
        }
    }
    return values_[index].str();
}

std::string As11SettingsState::pending_value(size_t index) const {
    if (!pending_values_ || index >= setting_capacity_) return "";
    return pending_values_[index].str();
}

bool As11SettingsState::setting_visible(size_t index, int mode) const {
    if (!feature_present_ || index >= setting_capacity_) return false;
    const As11SettingDef &def = catalog_.setting(index);
    if (!as11_setting_visible_for_mode(def, mode)) return false;
    if (def.source != As11SettingSource::FeatureProfile) return true;
    return feature_present_[index];
}

int As11SettingsState::mode_index() const {
    if (!values_) return -1;

    for (size_t i = 0; i < setting_capacity_; ++i) {
        if (setting_is_therapy_mode(catalog_.setting(i))) {
            return as11_mode_index_from_value(values_[i].str());
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

    if (def.source == As11SettingSource::Flat) {
        std::string out("_");
        out += def.key ? def.key : "";
        return out;
    }

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

bool as11_setting_readable_via_rpc(const As11SettingDef &def) {
    return def.source == As11SettingSource::Flat ||
           (def.source == As11SettingSource::TherapyProfile &&
            def.profile != As11ProfileId::None &&
            def.source_field != nullptr) ||
           (def.source == As11SettingSource::FeatureProfile &&
            def.source_object != nullptr && def.source_field != nullptr);
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

std::string as11_settings_get_params_json() {
    As11SettingsCatalog catalog;
    return as11_settings_get_params_json(catalog);
}

std::string as11_settings_get_params_json(
    const As11SettingsCatalog &catalog) {
    std::string out = "[";
    out += "\"_MOP\",\"TherapyProfiles\",\"FeatureProfiles\",\"_PHI\",";
    out += "\"AirbreakInfo\"";
    for (size_t i = 0; i < catalog.count(); ++i) {
        const As11SettingDef &def = catalog.setting(i);
        if (!def.mode_mask || !catalog.overlaid(def.key)) continue;
        if (rpc_name_matches_key(def.key, "MOP") ||
            rpc_name_matches_key(def.key, "PHI")) {
            continue;
        }

        out += ",\"_";
        out += def.key;
        out += '"';
    }
    out += ']';
    return out;
}

std::string as11_build_set_params_from_json(const std::string &body,
                                            int mode,
                                            size_t &accepted) {
    As11SettingsCatalog catalog;
    return as11_build_set_params_from_json(body, mode, accepted, catalog);
}

std::string as11_build_set_params_from_json(
    const std::string &body,
    int mode,
    size_t &accepted,
    const As11SettingsCatalog &catalog) {
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
    for (size_t i = 0; i < catalog.count(); ++i) {
        const As11SettingDef &def = catalog.setting(i);
        if (!def.writable || !as11_setting_visible_for_mode(def, mode)) {
            continue;
        }
        JsonVariantConst value = root[def.key];
        if (value.isNull()) continue;
        char key[80];
        const char *rpc_key = setting_rpc_key(def, key, sizeof(key));
        if (!rpc_key) continue;
        std::string literal;
        if (!json_literal_for_set(def, value, literal)) continue;
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
