#include "as11_settings_backup.h"

#include <cmath>
#include <stdlib.h>
#include <string.h>

#include "as11_device_state.h"
#include "large_json_allocator.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr const char *FORMAT = "aircannect-clinical-settings";

bool read_value(const As11SettingsState &state, size_t index, int mode,
                JsonVariant out) {
    const auto &def = state.catalog().setting(index);
    const std::string raw = state.value(index, mode);
    if (raw.empty()) return false;

    if (def.kind == As11SettingKind::Number) {
        out.set(strtod(raw.c_str(), nullptr) / (def.scale_div ? def.scale_div : 1));
    } else if (def.kind == As11SettingKind::Enum) {
        out.set(atoi(raw.c_str()));
    } else if (def.kind == As11SettingKind::Bool) {
        bool value = false;
        if (!parse_bool_yesno(raw, value)) return false;
        out.set(value);
    } else {
        out.set(raw);
    }
    return true;
}

std::string display_value(const As11SettingDef &def, JsonVariantConst value) {
    if (value.isNull()) return "--";
    if (def.kind == As11SettingKind::Enum && value.is<int>()) {
        const int index = value.as<int>();
        if (index >= 0 && index < def.option_count && def.options) {
            return def.options[index];
        }
    }
    return value.as<std::string>();
}

const char *reject_value(const As11SettingDef &def, JsonVariantConst value) {
    switch (def.kind) {
        case As11SettingKind::Number:
            if (!value.is<double>() || !std::isfinite(value.as<double>())) {
                return "value";
            }
            return value.as<double>() < def.min_value - 1e-6 ||
                   value.as<double>() > def.max_value + 1e-6 ? "range" : nullptr;
        case As11SettingKind::Enum:
            return !value.is<int>() || value.as<int>() < 0 ||
                   value.as<int>() >= def.option_count ? "value" : nullptr;
        case As11SettingKind::Bool:
            return value.is<bool>() ? nullptr : "value";
        case As11SettingKind::Text:
            return value.is<const char *>() ? nullptr : "value";
    }
    return "value";
}

}  // namespace

std::shared_ptr<const LargeByteBuffer> as11_settings_backup_encode(
    const As11SettingsState &state, const As11DeviceState &device,
    const char *created) {
    if (!state.valid() || state.pending_count() || state.mode_index() < 0) return {};

    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    document["format"] = FORMAT;
    document["version"] = 1;
    document["created"] = created;
    document["mode"] = state.mode_index();
    auto identity = document["device"].to<JsonObject>();
    identity["name"] = device.product_name();
    identity["serial"] = device.serial_number();
    identity["firmware"] = device.software_identifier();

    auto values = document["settings"].to<JsonObject>();
    const auto &catalog = state.catalog();
    for (size_t i = 0; i < catalog.count(); ++i) {
        const auto &def = catalog.setting(i);
        if (!catalog.supports(def) || !def.writable ||
            !state.setting_visible(i, state.mode_index()) ||
            !as11_setting_readable_via_rpc(def)) continue;

        if (!read_value(state, i, state.mode_index(), values[def.key].to<JsonVariant>())) {
            values.remove(def.key);
        }
    }
    if (document.overflowed() || !values["MOP"].is<int>()) return {};

    auto bytes = LargeByteBuffer::allocate(measureJson(document));
    if (!bytes || serializeJson(document, bytes->data(), bytes->size()) != bytes->size()) {
        return {};
    }
    return LargeByteBuffer::freeze(std::move(bytes));
}

const char *as11_settings_backup_prepare(
    const LargeByteBuffer &file, const As11SettingsState &state,
    As11SettingsBackupPlan &plan) {
    plan = {};
    if (!state.valid() || state.pending_count()) return "settings_not_ready";

    LargeJsonAllocator allocator;
    JsonDocument document(&allocator);
    if (deserializeJson(document, file.data(), file.size()) ||
        document["format"] != FORMAT || document["version"] != 1 ||
        !document["mode"].is<int>() || !document["settings"].is<JsonObject>()) {
        return "invalid_backup";
    }

    const int mode = document["mode"].as<int>();
    if (mode < 0 || mode >= static_cast<int>(As11SettingsState::MaxModes) ||
        !(state.supported_mode_mask() & (1u << mode))) return "unsupported_mode";

    const JsonObject values = document["settings"].as<JsonObject>();
    if (!values["MOP"].is<int>() || values["MOP"].as<int>() != mode) {
        return "invalid_backup";
    }
    plan.mode = mode;
    plan.source_serial = document["device"]["serial"] | "";
    plan.source_name = document["device"]["name"] | "";
    plan.created = document["created"] | "";

    JsonDocument changes(&allocator);
    changes.to<JsonObject>();
    JsonDocument current(&allocator);
    const auto &catalog = state.catalog();
    for (JsonPair entry : values) {
        BackupSettingChange row;
        row.key = entry.key().c_str();
        size_t index = 0;
        for (; index < catalog.count(); ++index) {
            const auto &candidate = catalog.setting(index);
            if (row.key == candidate.key && as11_setting_visible_for_mode(candidate, mode)) break;
        }
        if (index == catalog.count()) {
            row.reason = "unknown";
            row.before = "--";
            row.value = entry.value().as<std::string>();
        } else {
            const auto &def = catalog.setting(index);
            row.label = def.label;
            row.stock = !catalog.overlaid(def.key);
            if (!catalog.supports(def) || !as11_setting_readable_via_rpc(def)) {
                row.reason = "unknown";
            } else if (!def.writable) {
                row.reason = "readonly";
            } else {
                row.reason = reject_value(def, entry.value());
            }
            current.clear();
            const int read_mode = row.key == "MOP" ? state.mode_index() : mode;
            const bool available = read_value(state, index, read_mode, current.to<JsonVariant>());
            if (!available && !row.reason) row.reason = "unknown";
            row.before = display_value(def, current.as<JsonVariantConst>());
            row.value = display_value(def, entry.value());
            if (def.kind == As11SettingKind::Enum) {
                if (available) row.before_option = current.as<int>();
                if (!row.reason) row.value_option = entry.value().as<int>();
            }

            const bool same = def.kind == As11SettingKind::Number
                ? std::fabs(current.as<double>() - entry.value().as<double>()) <= 1e-6
                : current.as<JsonVariantConst>() == entry.value();
            if (!row.reason && same) {
                ++plan.unchanged;
                row.unchanged = true;
            }
        }
        if (!row.reason && !row.unchanged) changes[row.key] = entry.value();
        plan.entries.push_back(std::move(row));
    }
    if (changes.overflowed() || current.overflowed()) return "backup_alloc";

    As11SettingsWriteRequest request;
    if (!request.parse(changes.as<JsonObjectConst>())) return "invalid_backup";
    plan.write = as11_prepare_settings_write(request, mode, catalog);
    return nullptr;
}

}  // namespace aircannect
