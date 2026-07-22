#include "config_service.h"

#include "app_config_internal.h"
#include "debug_log.h"

#include <new>
#include <utility>

namespace aircannect {

ConfigService::~ConfigService() {
    clear_transaction();
}

bool ConfigService::begin() {
    const bool loaded = store_.begin();
    store_.apply_log_config();

    revision_ = 1;
    clear_transaction();
    return loaded;
}

void ConfigService::set_runtime_effects(ApplyRuntimeEffects apply,
                                        void *context) {
    apply_runtime_effects_ = apply;
    runtime_effects_context_ = context;
}

void ConfigService::activate_runtime_effects(bool apply_current) {
    runtime_effects_active_ = true;
    if (apply_current) publish_changes(AC_CONFIG_DIRTY_ALL);
}

bool ConfigService::begin_transaction() {
    if (transaction_active_) return false;

    transaction_store_ = new (std::nothrow) AppConfig(store_);
    if (!transaction_store_) return false;

    transaction_store_->begin_update();
    transaction_active_ = true;
    transaction_accepted_ = 0;
    transaction_changed_ = 0;
    transaction_dirty_ = 0;
    return true;
}

ConfigFieldUpdate ConfigService::set_transaction_value(
    const char *key,
    const String &value,
    bool preserve_secret_sentinel,
    bool provisionable_only) {
    ConfigFieldUpdate update;
    if (!transaction_active_) return update;

    const AppConfigFieldDescriptor *field = app_config_find_field(key);
    if (!field) {
        update.disposition = ConfigFieldDisposition::UnknownKey;
        return update;
    }
    if (provisionable_only &&
        (field->flags & AC_CONFIG_FIELD_PROVISIONABLE) == 0) {
        update.disposition = ConfigFieldDisposition::NotProvisionable;
        return update;
    }

    AppConfigStoreFieldResult field_result;
    if (!AppConfigFieldWriter::set_in_update(
            *transaction_store_, *field, value, preserve_secret_sentinel,
            field_result) ||
        !field_result.accepted) {
        update.disposition = ConfigFieldDisposition::InvalidValue;
        return update;
    }

    update.disposition = ConfigFieldDisposition::Accepted;
    update.changed = field_result.changed;
    update.dirty = field_result.dirty;

    transaction_accepted_++;
    if (field_result.changed) {
        transaction_changed_++;
        transaction_dirty_ |= field_result.dirty;
    }
    return update;
}

ConfigTransactionResult ConfigService::commit_transaction() {
    ConfigTransactionResult result;
    if (!transaction_active_) return result;

    result.accepted_fields = transaction_accepted_;
    result.changed_fields = transaction_changed_;
    result.dirty = transaction_dirty_;
    result.persisted = transaction_store_->commit_update();

    if (result.persisted && transaction_dirty_ != 0) {
        store_ = std::move(*transaction_store_);
        revision_++;
        if (revision_ == 0) revision_ = 1;
        publish_changes(transaction_dirty_);
    } else if (!result.persisted && transaction_dirty_ != 0 &&
               !store_.save_fields(transaction_dirty_)) {
        Log::logf(CAT_CONFIG, LOG_ERROR,
                  "failed to restore config after transaction error\n");
    }
    result.revision = revision_;

    clear_transaction();
    return result;
}

ConfigFieldUpdate ConfigService::set_value(
    const char *key,
    const String &value,
    bool preserve_secret_sentinel,
    ConfigTransactionResult *transaction) {
    ConfigFieldUpdate update;
    if (!begin_transaction()) return update;

    update = set_transaction_value(key, value, preserve_secret_sentinel);
    const ConfigTransactionResult committed = commit_transaction();
    if (transaction) *transaction = committed;
    return update;
}

ConfigTransactionResult ConfigService::reset() {
    ConfigTransactionResult result;
    if (transaction_active_) return result;

    AppConfig *candidate = new (std::nothrow) AppConfig(store_);
    if (!candidate) return result;

    result.accepted_fields = 1;
    result.dirty = AC_CONFIG_DIRTY_ALL;
    result.persisted = candidate->factory_reset();

    if (result.persisted) {
        store_ = std::move(*candidate);
        result.changed_fields = 1;

        revision_++;
        if (revision_ == 0) revision_ = 1;
        publish_changes(result.dirty);
    } else if (!store_.save_fields(AC_CONFIG_DIRTY_ALL)) {
        Log::logf(CAT_CONFIG, LOG_ERROR,
                  "failed to restore config after reset error\n");
    }
    delete candidate;

    result.revision = revision_;
    return result;
}

void ConfigService::clear_transaction() {
    delete transaction_store_;
    transaction_store_ = nullptr;
    transaction_active_ = false;
    transaction_accepted_ = 0;
    transaction_changed_ = 0;
    transaction_dirty_ = 0;
}

void ConfigService::publish_changes(uint32_t dirty) {
    if (dirty & (AC_CONFIG_DIRTY_HOSTNAME |
                 AC_CONFIG_DIRTY_LOG_LEVELS |
                 AC_CONFIG_DIRTY_SYSLOG |
                 AC_CONFIG_DIRTY_FILE_LOG)) {
        store_.apply_log_config();
    }

    if (!runtime_effects_active_ || !apply_runtime_effects_) return;
    apply_runtime_effects_(runtime_effects_context_, store_.data(), dirty);
}

}  // namespace aircannect
