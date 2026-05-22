#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace aircannect {

enum class As11SettingKind {
    Number,
    Enum,
    Bool,
    Text,
};

class As11StoredValue {
public:
    As11StoredValue() = default;
    As11StoredValue(const As11StoredValue &other);
    As11StoredValue(As11StoredValue &&other) noexcept;
    ~As11StoredValue();

    As11StoredValue &operator=(const As11StoredValue &other);
    As11StoredValue &operator=(As11StoredValue &&other) noexcept;

    bool set(const std::string &value);
    void clear();
    bool empty() const { return length_ == 0; }
    const char *c_str() const { return heap_ ? heap_ : inline_; }
    std::string str() const { return std::string(c_str(), length_); }

private:
    static constexpr size_t InlineCapacity = 23;

    char inline_[InlineCapacity + 1] = {};
    char *heap_ = nullptr;
    size_t length_ = 0;
};

struct As11SettingDef {
    const char *name;
    const char *profile_field;
    const char *feature_name;
    const char *feature_field;
    const char *flat_name;
    const char *label;
    const char *group;
    As11SettingKind kind;
    float min_value;
    float max_value;
    float step;
    const char *const *options;
    uint8_t option_count;
    uint16_t mode_mask;
    uint16_t scale_div;
    uint8_t decimals;
};

class As11SettingsState {
public:
    static constexpr size_t MaxSettings = 64;
    static constexpr size_t MaxModes = 11;
    static constexpr size_t MaxProfileValues = 128;

    bool apply_settings_get_response(const std::string &payload,
                                     uint32_t now_ms);
    bool note_set_request(const std::string &params_json, uint32_t now_ms);
    void note_set_response(bool is_error, uint32_t now_ms);
    void note_set_cancelled(const char *reason, uint32_t now_ms);

    void clear();

    bool valid() const { return valid_; }
    uint32_t updated_ms() const { return updated_ms_; }

    const std::string &value(size_t index) const { return values_[index]; }
    std::string value(size_t index, int mode) const;
    bool pending(size_t index) const { return pending_[index]; }
    const std::string &pending_value(size_t index) const {
        return pending_values_[index];
    }
    uint32_t pending_since_ms(size_t index) const {
        return pending_since_ms_[index];
    }
    size_t pending_count() const { return pending_count_; }
    const std::string &last_write_status() const {
        return last_write_status_;
    }
    uint32_t last_write_ms() const { return last_write_ms_; }

    int mode_index() const;
    uint16_t supported_mode_mask() const { return supported_mode_mask_; }
    bool feature_present(size_t index) const;
    bool setting_visible(size_t index, int mode) const;

private:
    struct ProfileValueSlot {
        bool used = false;
        uint8_t mode = 0;
        uint8_t index = 0;
        As11StoredValue value;
    };

    void clear_pending(size_t index);
    void clear_all_pending();
    void clear_profile_values();
    const As11StoredValue *profile_value(size_t index, int mode) const;
    bool set_profile_value(int mode,
                           size_t index,
                           const std::string &value);

    std::string values_[MaxSettings];
    ProfileValueSlot profile_values_[MaxProfileValues];
    bool feature_present_[MaxSettings] = {};

    std::string pending_values_[MaxSettings];
    uint32_t pending_since_ms_[MaxSettings] = {};
    bool pending_[MaxSettings] = {};
    size_t pending_count_ = 0;

    std::string last_write_status_;
    uint32_t last_write_ms_ = 0;
    uint32_t updated_ms_ = 0;
    uint16_t supported_mode_mask_ = 0;
    bool valid_ = false;
};

size_t as11_setting_count();
const As11SettingDef &as11_setting(size_t index);
const As11SettingDef *as11_find_setting(const char *name);

bool as11_setting_visible_for_mode(const As11SettingDef &def, int mode);
bool as11_setting_option_supported(const As11SettingDef &def,
                                   uint8_t option_index,
                                   uint16_t supported_mode_mask);
bool as11_setting_readable_via_rpc(const As11SettingDef &def);
bool as11_setting_writable_via_rpc(const As11SettingDef &def, int mode);
int as11_mode_index_from_value(const std::string &value);
const char *as11_mode_name(int mode);

std::string as11_setting_display_value(const As11SettingDef &def,
                                       const std::string &raw);
std::string as11_settings_get_params_json(int mode);
std::string as11_build_set_params_from_json(const std::string &body,
                                            int mode,
                                            size_t &accepted);

}  // namespace aircannect
