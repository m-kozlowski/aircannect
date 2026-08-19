#include "as11_settings.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr size_t NO_STOCK_SETTING = SIZE_MAX;

void *catalog_alloc(size_t size) {
#ifdef ARDUINO
    return Memory::calloc_large(1, size, false);
#else
    return calloc(1, size);
#endif
}

void catalog_free(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool selector_key(const char *selector, const char *&key, size_t &key_len) {
    if (!selector || selector[0] != '_') return false;

    key = selector + 1;
    key_len = strlen(key);
    if (key_len == 0 || key_len > 6) return false;

    for (size_t i = 0; i < key_len; ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (!isalnum(c)) return false;
    }
    return true;
}

const char *clinical_group(const char *section) {
    if (!section) return nullptr;
    if (strcasecmp(section, "therapy") == 0) return "Therapy";
    if (strcasecmp(section, "comfort") == 0) return "Comfort";
    if (strcasecmp(section, "circuit") == 0) return "Circuit";
    if (strcasecmp(section, "configuration") == 0) return "Configuration";
    if (strcasecmp(section, "preferences") == 0) return "Preferences";
    if (strcasecmp(section, "device") == 0) return "Device";
    return nullptr;
}

uint16_t menu_mode_mask(JsonObjectConst item) {
    JsonArrayConst modes = item["menu"]["modes"].as<JsonArrayConst>();
    uint16_t mask = 0;
    for (JsonVariantConst value : modes) {
        if (!value.is<const char *>()) continue;
        const int mode = as11_mode_index_from_value(
            value.as<const char *>());
        if (mode >= 0) mask |= static_cast<uint16_t>(1u << mode);
    }
    return mask;
}

bool editable_enum(JsonObjectConst item, size_t &count) {
    JsonArrayConst values = item["enum"].as<JsonArrayConst>();
    count = values.size();
    if (count == 0 || count > UINT8_MAX) return false;

    for (JsonVariantConst value : values) {
        if (!value.is<const char *>()) return false;
    }
    return true;
}

size_t stock_setting_index(const char *key) {
    for (size_t i = 0; i < as11_setting_count(); ++i) {
        const As11SettingDef &def = as11_setting(i);
        if (def.key && strcmp(def.key, key) == 0) return i;
    }
    return NO_STOCK_SETTING;
}

bool same_text(const char *a, const char *b) {
    return strcmp(a ? a : "", b ? b : "") == 0;
}

}  // namespace

struct As11SettingsCatalog::RuntimeItem {
    As11SettingDef def;
    size_t stock_index = NO_STOCK_SETTING;
};

As11SettingsCatalog::~As11SettingsCatalog() {
    clear_overlay();
}

size_t As11SettingsCatalog::count() const {
    size_t appended = 0;
    for (size_t i = 0; i < item_count_; ++i) {
        if (items_[i].stock_index == NO_STOCK_SETTING) appended++;
    }
    return as11_setting_count() + appended;
}

const As11SettingDef &As11SettingsCatalog::setting(size_t index) const {
    if (index < as11_setting_count()) {
        for (size_t i = 0; i < item_count_; ++i) {
            if (items_[i].stock_index == index) return items_[i].def;
        }
        return as11_setting(index);
    }

    size_t appended_index = index - as11_setting_count();
    for (size_t i = 0; i < item_count_; ++i) {
        if (items_[i].stock_index != NO_STOCK_SETTING) continue;
        if (appended_index-- == 0) return items_[i].def;
    }
    return as11_setting(0);
}

const As11SettingDef *As11SettingsCatalog::find(const char *key) const {
    if (!key) return nullptr;
    if (key[0] == '_') key++;

    for (size_t i = 0; i < item_count_; ++i) {
        if (items_[i].def.key && strcmp(items_[i].def.key, key) == 0) {
            return &items_[i].def;
        }
    }
    return as11_find_setting(key);
}

bool As11SettingsCatalog::overlaid(const char *key) const {
    if (!key) return false;
    if (key[0] == '_') key++;

    for (size_t i = 0; i < item_count_; ++i) {
        if (items_[i].def.key && strcmp(items_[i].def.key, key) == 0) {
            return true;
        }
    }
    return false;
}

bool As11SettingsCatalog::apply_airbreak_info(JsonObjectConst info,
                                               bool &changed) {
    changed = false;
    As11SettingsCatalog candidate;

    JsonObjectConst data_items;
    if (!info.isNull() && info["schema"].as<int>() == 4) {
        data_items = info["dataItems"].as<JsonObjectConst>();
    }

    size_t item_count = 0;
    size_t option_count = 0;
    size_t string_bytes = 0;
    for (JsonPairConst pair : data_items) {
        const char *key = nullptr;
        size_t key_len = 0;
        JsonObjectConst item = pair.value().as<JsonObjectConst>();
        if (item.isNull() ||
            !selector_key(pair.key().c_str(), key, key_len)) {
            continue;
        }

        const char *name = item["name"].as<const char *>();
        const char *group = clinical_group(
            item["menu"]["section"].as<const char *>());
        const uint16_t mode_mask = group ? menu_mode_mask(item) : 0;
        const size_t stock_index = stock_setting_index(key);
        if (stock_index == NO_STOCK_SETTING && mode_mask == 0) continue;

        size_t item_options = 0;
        const bool has_enum = editable_enum(item, item_options);

        item_count++;
        option_count += has_enum ? item_options : 0;
        string_bytes += key_len + 1;
        string_bytes += strlen(name ? name : key) + 1;
        string_bytes += strlen(group ? group : "") + 1;
        if (has_enum) {
            for (JsonVariantConst value : item["enum"].as<JsonArrayConst>()) {
                string_bytes += strlen(value.as<const char *>()) + 1;
            }
        }
    }

    if (item_count) {
        const size_t items_bytes = sizeof(RuntimeItem) * item_count;
        const size_t options_offset = align_up(items_bytes,
                                               alignof(const char *));
        const size_t options_bytes = sizeof(const char *) * option_count;
        if (options_offset > SIZE_MAX - options_bytes ||
            options_offset + options_bytes > SIZE_MAX - string_bytes) {
            return false;
        }

        const size_t allocation_size =
            options_offset + options_bytes + string_bytes;
        candidate.allocation_ = catalog_alloc(allocation_size);
        if (!candidate.allocation_) return false;

        candidate.items_ = static_cast<RuntimeItem *>(candidate.allocation_);
        candidate.item_count_ = item_count;
        const char **option_cursor = reinterpret_cast<const char **>(
            static_cast<uint8_t *>(candidate.allocation_) + options_offset);
        char *string_cursor = reinterpret_cast<char *>(option_cursor +
                                                       option_count);

        auto copy_text = [&string_cursor](const char *text) {
            const char *source = text ? text : "";
            const size_t length = strlen(source) + 1;
            char *stored = string_cursor;
            memcpy(stored, source, length);
            string_cursor += length;
            return static_cast<const char *>(stored);
        };

        size_t output_index = 0;
        for (JsonPairConst pair : data_items) {
            const char *key = nullptr;
            size_t key_len = 0;
            JsonObjectConst item = pair.value().as<JsonObjectConst>();
            if (item.isNull() ||
                !selector_key(pair.key().c_str(), key, key_len)) {
                continue;
            }

            const char *group = clinical_group(
                item["menu"]["section"].as<const char *>());
            const uint16_t mode_mask = group ? menu_mode_mask(item) : 0;
            const size_t stock_index = stock_setting_index(key);
            if (stock_index == NO_STOCK_SETTING && mode_mask == 0) continue;

            RuntimeItem &output = candidate.items_[output_index++];
            const char *stored_key = copy_text(key);
            const char *name = item["name"].as<const char *>();
            const char *stored_name = copy_text(name ? name : key);
            const char *stored_group = copy_text(group ? group : "");

            size_t item_options = 0;
            const bool has_enum = editable_enum(item, item_options);
            const char **stored_options = has_enum ? option_cursor : nullptr;
            if (has_enum) {
                for (JsonVariantConst value :
                     item["enum"].as<JsonArrayConst>()) {
                    *option_cursor++ = copy_text(value.as<const char *>());
                }
            }

            output.stock_index = stock_index;
            output.def = {
                stored_key,
                As11SettingSource::Flat,
                As11ProfileId::None,
                nullptr,
                stored_key,
                stored_name,
                stored_group,
                "airbreak",
                has_enum ? As11SettingKind::Enum : As11SettingKind::Text,
                0.0f,
                0.0f,
                1.0f,
                stored_options,
                static_cast<uint8_t>(has_enum ? item_options : 0),
                mode_mask,
                1,
                0,
                stored_options,
                has_enum,
            };
        }
    }

    if (equivalent(candidate)) return true;

    candidate.revision_ = revision_ + 1;
    if (candidate.revision_ == 0) candidate.revision_ = 1;
    swap(candidate);
    changed = true;
    return true;
}

bool As11SettingsCatalog::equivalent(
    const As11SettingsCatalog &other) const {
    if (item_count_ != other.item_count_) return false;

    for (size_t i = 0; i < item_count_; ++i) {
        const RuntimeItem &a = items_[i];
        const RuntimeItem &b = other.items_[i];
        if (a.stock_index != b.stock_index ||
            a.def.kind != b.def.kind ||
            a.def.mode_mask != b.def.mode_mask ||
            a.def.option_count != b.def.option_count ||
            a.def.writable != b.def.writable ||
            !same_text(a.def.key, b.def.key) ||
            !same_text(a.def.label, b.def.label) ||
            !same_text(a.def.group, b.def.group)) {
            return false;
        }

        for (uint8_t option = 0; option < a.def.option_count; ++option) {
            if (!same_text(a.def.options[option], b.def.options[option])) {
                return false;
            }
        }
    }
    return true;
}

void As11SettingsCatalog::clear_overlay() {
    catalog_free(allocation_);
    allocation_ = nullptr;
    items_ = nullptr;
    item_count_ = 0;
}

void As11SettingsCatalog::swap(As11SettingsCatalog &other) {
    void *allocation = allocation_;
    allocation_ = other.allocation_;
    other.allocation_ = allocation;

    RuntimeItem *items = items_;
    items_ = other.items_;
    other.items_ = items;

    size_t item_count = item_count_;
    item_count_ = other.item_count_;
    other.item_count_ = item_count;

    uint32_t revision = revision_;
    revision_ = other.revision_;
    other.revision_ = revision;
}

}  // namespace aircannect
