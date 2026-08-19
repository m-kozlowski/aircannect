#pragma once

#include <array>
#include <stddef.h>
#include <stdint.h>

#include "as11_settings.h"

namespace aircannect {

template <typename T, size_t N>
constexpr uint8_t option_count(const T (&)[N]) {
    return static_cast<uint8_t>(N);
}

struct As11SettingValueDef {
    As11SettingKind kind;
    float min_value;
    float max_value;
    float step;
    const char *const *options;
    uint8_t option_count;
    uint16_t scale_div;
    uint8_t decimals;
    const char *const *wire_options;
    bool writable;
};

constexpr As11SettingValueDef number_value(float min_value,
                                           float max_value,
                                           float step,
                                           uint16_t scale_div,
                                           uint8_t decimals) {
    return {As11SettingKind::Number, min_value, max_value, step,
            nullptr, 0, scale_div, decimals, nullptr, true};
}

template <size_t N>
constexpr As11SettingValueDef enum_value(
    const char *const (&options)[N]) {
    return {As11SettingKind::Enum, 0.0f, 0.0f, 1.0f, options,
            option_count(options), 1, 0, nullptr, true};
}

inline As11SettingValueDef enum_value(const char *const *options,
                                      uint8_t count) {
    return {As11SettingKind::Enum, 0.0f, 0.0f, 1.0f, options,
            count, 1, 0, nullptr, true};
}

template <size_t N, size_t M>
constexpr As11SettingValueDef enum_value(
    const char *const (&options)[N],
    const char *const (&wire_options)[M]) {
    static_assert(N == M, "display and wire option counts must match");
    return {As11SettingKind::Enum, 0.0f, 0.0f, 1.0f, options,
            option_count(options), 1, 0, wire_options, true};
}

template <size_t N, size_t M>
constexpr As11SettingValueDef ranged_enum_value(
    const char *const (&options)[N],
    const char *const (&wire_options)[M],
    float min_value,
    float max_value,
    float step) {
    static_assert(N == M, "display and wire option counts must match");
    return {As11SettingKind::Enum, min_value, max_value, step, options,
            option_count(options), 1, 0, wire_options, true};
}

template <size_t N, size_t M>
constexpr As11SettingValueDef ranged_enum_value(
    const std::array<const char *, N> &options,
    const std::array<const char *, M> &wire_options,
    float min_value,
    float max_value,
    float step) {
    static_assert(N == M, "display and wire option counts must match");
    return {As11SettingKind::Enum, min_value, max_value, step,
            options.data(), static_cast<uint8_t>(N), 1, 0,
            wire_options.data(), true};
}

constexpr As11SettingValueDef text_value(bool writable) {
    return {As11SettingKind::Text, 0.0f, 0.0f, 1.0f,
            nullptr, 0, 1, 0, nullptr, writable};
}

constexpr As11SettingDef setting_definition(
    const char *key,
    As11SettingSource source,
    As11ProfileId profile,
    const char *source_object,
    const char *source_field,
    const char *label,
    const char *group,
    const char *category,
    uint16_t mode_mask,
    As11SettingValueDef value) {
    return {
        key, source, profile, source_object, source_field,
        label, group, category,
        value.kind, value.min_value, value.max_value, value.step,
        value.options, value.option_count, mode_mask, value.scale_div,
        value.decimals, value.wire_options, value.writable,
    };
}

constexpr As11SettingDef flat_setting(
    const char *key,
    const char *source_field,
    const char *label,
    const char *group,
    const char *category,
    uint16_t mode_mask,
    As11SettingValueDef value) {
    return setting_definition(key, As11SettingSource::Flat,
                              As11ProfileId::None, nullptr, source_field,
                              label, group, category, mode_mask, value);
}

constexpr As11SettingDef therapy_setting(
    const char *key,
    As11ProfileId profile,
    const char *source_field,
    const char *label,
    const char *group,
    const char *category,
    uint16_t mode_mask,
    As11SettingValueDef value) {
    return setting_definition(key, As11SettingSource::TherapyProfile,
                              profile, nullptr, source_field, label, group,
                              category, mode_mask, value);
}

constexpr As11SettingDef feature_setting(
    const char *key,
    const char *source_object,
    const char *source_field,
    const char *label,
    const char *group,
    const char *category,
    uint16_t mode_mask,
    As11SettingValueDef value) {
    return setting_definition(key, As11SettingSource::FeatureProfile,
                              As11ProfileId::None, source_object,
                              source_field, label, group, category,
                              mode_mask, value);
}

}  // namespace aircannect
