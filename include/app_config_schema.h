#pragma once

#include <stdint.h>

namespace aircannect {

static constexpr uint32_t AC_CONFIG_SCHEMA_VERSION = 18;

enum class AppConfigSchemaMode : uint8_t {
    Initialize,
    Supported,
    CompatibleRollback,
};

enum class AppConfigWriteMode : uint8_t {
    Complete,
    ChangedFields,
};

constexpr AppConfigSchemaMode app_config_schema_mode(uint32_t stored_schema) {
    if (stored_schema == 0) return AppConfigSchemaMode::Initialize;
    if (stored_schema > AC_CONFIG_SCHEMA_VERSION) {
        return AppConfigSchemaMode::CompatibleRollback;
    }
    return AppConfigSchemaMode::Supported;
}

constexpr bool app_config_schema_loads_known_fields(
    AppConfigSchemaMode mode) {
    return mode != AppConfigSchemaMode::Initialize;
}

constexpr bool app_config_schema_allows_automatic_rewrite(
    AppConfigSchemaMode mode) {
    return mode == AppConfigSchemaMode::Supported;
}

constexpr bool app_config_write_includes_schema(AppConfigWriteMode mode) {
    return mode == AppConfigWriteMode::Complete;
}

constexpr bool app_config_write_includes_field(AppConfigWriteMode mode,
                                               bool changed) {
    return mode == AppConfigWriteMode::Complete || changed;
}

}  // namespace aircannect
