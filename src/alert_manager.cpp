#include "alert_manager.h"

namespace aircannect {

AlertManager::AlertManager() {
    scope_active_[static_cast<size_t>(AlertScope::System)] = true;
}

bool AlertManager::add_sink(AlertSink &sink) {
    for (size_t i = 0; i < sink_count_; ++i) {
        if (sinks_[i] == &sink) return true;
    }
    if (sink_count_ >= MAX_SINKS) return false;

    sinks_[sink_count_++] = &sink;
    return true;
}

bool AlertManager::configure_threshold(const AlertThresholdRule &rule,
                                       uint32_t now_ms) {
    if (rule.kind >= AlertKind::Count ||
        rule.metric >= AlertMetric::Count ||
        rule.scope >= AlertScope::Count) {
        return false;
    }

    RuleSlot *slot = find_rule(rule.kind);
    if (!slot) slot = find_free_rule();
    if (!slot) return false;

    const bool same_identity =
        slot->occupied && slot->config.metric == rule.metric &&
        slot->config.scope == rule.scope;
    if (slot->occupied && !same_identity &&
        state_has_active_incident(slot->state)) {
        clear(*slot, AlertClearReason::Disabled, now_ms);
    }

    slot->config = rule;
    slot->occupied = true;
    if (!rule.enabled) {
        if (state_has_active_incident(slot->state)) {
            clear(*slot, AlertClearReason::Disabled, now_ms);
        }
        slot->state = RuleState::Idle;
        return true;
    }

    if (!same_identity || slot->state == RuleState::PendingRaise) {
        slot->state = RuleState::Idle;
    }
    return true;
}

void AlertManager::set_scope_active(AlertScope scope,
                                    bool active,
                                    uint32_t now_ms) {
    if (scope >= AlertScope::Count) return;

    const size_t index = static_cast<size_t>(scope);
    if (scope_active_[index] == active) return;
    scope_active_[index] = active;
    if (active) return;

    for (RuleSlot &slot : rules_) {
        if (!slot.occupied || slot.config.scope != scope) continue;
        if (state_has_active_incident(slot.state)) {
            clear(slot, AlertClearReason::ContextEnded, now_ms);
        }
        slot.state = RuleState::Idle;
    }
}

void AlertManager::observe(AlertMetric metric,
                           bool valid,
                           float value,
                           uint32_t now_ms) {
    if (metric >= AlertMetric::Count) return;

    for (RuleSlot &slot : rules_) {
        if (!slot.occupied || slot.config.metric != metric) continue;
        evaluate(slot, valid, value, now_ms);
    }
}

bool AlertManager::state_has_active_incident(RuleState state) {
    return state == RuleState::Active || state == RuleState::PendingClear;
}

bool AlertManager::raise_condition(const AlertThresholdRule &rule,
                                   float value) {
    return rule.direction == AlertDirection::Above
               ? value >= rule.raise_threshold
               : value <= rule.raise_threshold;
}

bool AlertManager::clear_condition(const AlertThresholdRule &rule,
                                   float value) {
    return rule.direction == AlertDirection::Above
               ? value <= rule.clear_threshold
               : value >= rule.clear_threshold;
}

AlertManager::RuleSlot *AlertManager::find_rule(AlertKind kind) {
    for (RuleSlot &slot : rules_) {
        if (slot.occupied && slot.config.kind == kind) return &slot;
    }
    return nullptr;
}

AlertManager::RuleSlot *AlertManager::find_free_rule() {
    for (RuleSlot &slot : rules_) {
        if (!slot.occupied) return &slot;
    }
    return nullptr;
}

void AlertManager::evaluate(RuleSlot &slot,
                            bool valid,
                            float value,
                            uint32_t now_ms) {
    if (!slot.config.enabled) return;

    const size_t scope_index = static_cast<size_t>(slot.config.scope);
    if (!scope_active_[scope_index]) {
        if (state_has_active_incident(slot.state)) {
            clear(slot, AlertClearReason::ContextEnded, now_ms);
        }
        slot.state = RuleState::Idle;
        return;
    }

    if (!valid) {
        if (state_has_active_incident(slot.state)) {
            clear(slot, AlertClearReason::SourceLost, now_ms);
        }
        slot.state = RuleState::Idle;
        return;
    }

    slot.last_value = value;
    const bool should_raise = raise_condition(slot.config, value);
    const bool should_clear = clear_condition(slot.config, value);

    switch (slot.state) {
        case RuleState::Idle:
            if (!should_raise) return;
            slot.state = RuleState::PendingRaise;
            slot.transition_ms = now_ms;
            if (slot.config.raise_delay_ms == 0) raise(slot, value, now_ms);
            return;

        case RuleState::PendingRaise:
            if (!should_raise) {
                slot.state = RuleState::Idle;
                return;
            }
            if (now_ms - slot.transition_ms >= slot.config.raise_delay_ms) {
                raise(slot, value, now_ms);
            }
            return;

        case RuleState::Active:
            if (!should_clear) return;
            slot.state = RuleState::PendingClear;
            slot.transition_ms = now_ms;
            if (slot.config.clear_delay_ms == 0) {
                clear(slot, AlertClearReason::Recovered, now_ms);
            }
            return;

        case RuleState::PendingClear:
            if (!should_clear) {
                slot.state = RuleState::Active;
                return;
            }
            if (now_ms - slot.transition_ms >= slot.config.clear_delay_ms) {
                clear(slot, AlertClearReason::Recovered, now_ms);
            }
            return;
    }
}

void AlertManager::raise(RuleSlot &slot,
                         float value,
                         uint32_t now_ms) {
    slot.state = RuleState::Active;
    slot.incident_id = next_incident_id_++;
    if (next_incident_id_ == 0) next_incident_id_ = 1;

    AlertEvent event;
    event.kind = slot.config.kind;
    event.metric = slot.config.metric;
    event.scope = slot.config.scope;
    event.severity = slot.config.severity;
    event.state = AlertState::Raised;
    event.incident_id = slot.incident_id;
    event.occurred_ms = now_ms;
    event.value = value;
    event.threshold = slot.config.raise_threshold;
    publish(event);
}

void AlertManager::clear(RuleSlot &slot,
                         AlertClearReason reason,
                         uint32_t now_ms) {
    AlertEvent event;
    event.kind = slot.config.kind;
    event.metric = slot.config.metric;
    event.scope = slot.config.scope;
    event.severity = slot.config.severity;
    event.state = AlertState::Cleared;
    event.clear_reason = reason;
    event.incident_id = slot.incident_id;
    event.occurred_ms = now_ms;
    event.value = slot.last_value;
    event.threshold = slot.config.raise_threshold;
    publish(event);

    slot.state = RuleState::Idle;
    slot.incident_id = 0;
}

void AlertManager::publish(const AlertEvent &event) {
    for (size_t i = 0; i < sink_count_; ++i) {
        sinks_[i]->accept(event);
    }
}

const char *alert_kind_name(AlertKind kind) {
    switch (kind) {
        case AlertKind::HighLeak: return "high_leak";
        case AlertKind::LowSpo2: return "low_spo2";
        case AlertKind::Count: break;
    }
    return "unknown";
}

const char *alert_clear_reason_name(AlertClearReason reason) {
    switch (reason) {
        case AlertClearReason::None: return "none";
        case AlertClearReason::Recovered: return "recovered";
        case AlertClearReason::SourceLost: return "source_lost";
        case AlertClearReason::ContextEnded: return "context_ended";
        case AlertClearReason::Disabled: return "disabled";
    }
    return "unknown";
}

}  // namespace aircannect
