#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>

#include "app_config.h"
#include "app_config_registry.h"

namespace aircannect {

enum class ConfigFieldDisposition : uint8_t {
    Accepted,
    UnknownKey,
    NotProvisionable,
    InvalidValue,
    TransactionInactive,
};

struct ConfigFieldUpdate {
    ConfigFieldDisposition disposition =
        ConfigFieldDisposition::TransactionInactive;
    bool changed = false;
    uint32_t dirty = 0;

    bool accepted() const {
        return disposition == ConfigFieldDisposition::Accepted;
    }
};

struct ConfigTransactionResult {
    size_t accepted_fields = 0;
    size_t changed_fields = 0;
    uint32_t dirty = 0;
    uint32_t revision = 0;
    bool persisted = false;
};

class ConfigService {
public:
    using ApplyRuntimeEffects = void (*)(void *context,
                                         const AppConfigData &config,
                                         uint32_t dirty);

    // Lifecycle and immutable view
    ConfigService() = default;
    ~ConfigService();
    ConfigService(const ConfigService &) = delete;
    ConfigService &operator=(const ConfigService &) = delete;

    bool begin();
    const AppConfigData &data() const { return store_.data(); }
    uint32_t revision() const { return revision_; }

    // Runtime publication
    void set_runtime_effects(ApplyRuntimeEffects apply, void *context);
    void activate_runtime_effects(bool apply_current = true);

    // Strict transactions
    bool begin_transaction();
    ConfigFieldUpdate set_transaction_value(const char *key,
                                             const String &value,
                                             bool preserve_secret_sentinel,
                                             bool provisionable_only = false);
    ConfigTransactionResult commit_transaction();

    ConfigFieldUpdate set_value(const char *key,
                                const String &value,
                                bool preserve_secret_sentinel,
                                ConfigTransactionResult *transaction = nullptr);
    ConfigTransactionResult reset();

private:
    void clear_transaction();
    void publish_changes(uint32_t dirty);

    AppConfig store_;

    ApplyRuntimeEffects apply_runtime_effects_ = nullptr;
    void *runtime_effects_context_ = nullptr;
    bool runtime_effects_active_ = false;

    bool transaction_active_ = false;
    AppConfig *transaction_store_ = nullptr;
    size_t transaction_accepted_ = 0;
    size_t transaction_changed_ = 0;
    uint32_t transaction_dirty_ = 0;
    uint32_t revision_ = 0;
};

}  // namespace aircannect
