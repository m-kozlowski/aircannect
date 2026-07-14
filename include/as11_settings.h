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

enum class As11SettingSource {
    Flat,
    TherapyProfile,
    FeatureProfile,
};

enum class As11ProfileId : uint8_t {
    Cpap = 0,
    AutoSet = 1,
    HerAuto = 2,
    Spont = 3,
    ST = 4,
    Timed = 5,
    VAuto = 6,
    ASV = 7,
    ASVAuto = 8,
    iVAPS = 9,
    PAC = 10,
    None = 255,
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
    const char *key;
    As11SettingSource source;
    As11ProfileId profile;
    // FeatureProfiles object path. TherapyProfiles use profile instead.
    const char *source_object;
    const char *source_field;
    const char *label;
    const char *group;
    const char *category;
    As11SettingKind kind;
    float min_value;
    float max_value;
    float step;
    const char *const *options;
    uint8_t option_count;
    uint16_t mode_mask;
    uint16_t scale_div;
    uint8_t decimals;
    // Optional protocol literals when display labels differ from AS11 values.
    const char *const *wire_options = nullptr;
};

struct As11SettingCompositeOption {
    const char *label;
    int16_t enum_value;
    const char *numeric_raw;
};

struct As11SettingCompositeDef {
    const char *key;
    const char *label;
    const char *enum_key;
    const char *numeric_key;
    int16_t numeric_branch_enum_value;
    const char *group;
    const char *category;
    const As11SettingCompositeOption *options;
    uint8_t option_count;
};

bool as11_setting_option_index_for_rpc_name(const char *rpc_name,
                                            const char *wire_value,
                                            int16_t &index);

class As11SettingsState {
public:
    static constexpr size_t MaxModes = 11;
    static constexpr size_t MaxProfileValues = 128;

    As11SettingsState() = default;
    As11SettingsState(const As11SettingsState &) = delete;
    As11SettingsState &operator=(const As11SettingsState &) = delete;
    ~As11SettingsState();

    bool apply_settings_get_response(const std::string &payload,
                                     uint32_t now_ms);
    bool note_set_request(const std::string &params_json, uint32_t now_ms);
    void note_set_response(bool is_error, uint32_t now_ms);
    void note_set_cancelled(const char *reason, uint32_t now_ms);

    void clear();

    bool valid() const { return valid_; }
    uint32_t updated_ms() const { return updated_ms_; }

    std::string value(size_t index) const;
    std::string value(size_t index, int mode) const;
    bool pending(size_t index) const {
        return pending_ && index < setting_capacity_ && pending_[index];
    }
    std::string pending_value(size_t index) const;
    uint32_t pending_since_ms(size_t index) const {
        if (!pending_since_ms_ || index >= setting_capacity_) return 0;
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

    bool ensure_storage();
    void release_storage();

    void clear_pending(size_t index);
    void clear_all_pending();
    void clear_profile_values();
    const As11StoredValue *profile_value(size_t index, int mode) const;
    bool set_profile_value(int mode,
                           size_t index,
                           const std::string &value);

    As11StoredValue *values_ = nullptr;
    As11StoredValue *pending_values_ = nullptr;
    ProfileValueSlot *profile_values_ = nullptr;

    bool *feature_present_ = nullptr;
    bool *pending_ = nullptr;
    uint32_t *pending_since_ms_ = nullptr;

    size_t setting_capacity_ = 0;
    size_t profile_capacity_ = 0;
    size_t pending_count_ = 0;

    std::string last_write_status_;
    uint32_t last_write_ms_ = 0;
    uint32_t updated_ms_ = 0;
    uint16_t supported_mode_mask_ = 0;
    bool valid_ = false;
};

size_t as11_setting_count();
const As11SettingDef &as11_setting(size_t index);
const As11SettingDef *as11_find_setting(const char *key);
std::string as11_setting_rpc_long_name(const As11SettingDef &def);

size_t as11_setting_composite_count();
const As11SettingCompositeDef &as11_setting_composite(size_t index);

bool as11_setting_visible_for_mode(const As11SettingDef &def, int mode);
bool as11_setting_readable_via_rpc(const As11SettingDef &def);
bool as11_setting_writable_via_rpc(const As11SettingDef &def);
int as11_mode_index_from_value(const std::string &value);

std::string as11_settings_get_params_json();
std::string as11_build_set_params_from_json(const std::string &body,
                                            int mode,
                                            size_t &accepted);

}  // namespace aircannect
