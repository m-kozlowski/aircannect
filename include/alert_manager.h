#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class AlertMetric : uint8_t {
    Leak,
    Spo2,
    Pulse,
    Count,
};

enum class AlertKind : uint8_t {
    HighLeak,
    LowSpo2,
    Count,
};

enum class AlertScope : uint8_t {
    System,
    TherapySession,
    OximetrySession,
    Count,
};

enum class AlertSeverity : uint8_t {
    Info,
    Warning,
    Critical,
};

enum class AlertDirection : uint8_t {
    Above,
    Below,
};

enum class AlertState : uint8_t {
    Raised,
    Cleared,
};

enum class AlertClearReason : uint8_t {
    None,
    Recovered,
    SourceLost,
    ContextEnded,
    Disabled,
};

struct AlertEvent {
    AlertKind kind = AlertKind::HighLeak;
    AlertMetric metric = AlertMetric::Leak;
    AlertScope scope = AlertScope::System;
    AlertSeverity severity = AlertSeverity::Warning;
    AlertState state = AlertState::Raised;
    AlertClearReason clear_reason = AlertClearReason::None;
    uint32_t incident_id = 0;
    uint32_t occurred_ms = 0;
    float value = 0.0f;
    float threshold = 0.0f;
};

class AlertSink {
public:
    virtual ~AlertSink() = default;

    // Called from the main loop. Implementations must not block.
    virtual void accept(const AlertEvent &event) = 0;
};

struct AlertThresholdRule {
    AlertKind kind = AlertKind::HighLeak;
    AlertMetric metric = AlertMetric::Leak;
    AlertScope scope = AlertScope::TherapySession;
    AlertSeverity severity = AlertSeverity::Warning;
    AlertDirection direction = AlertDirection::Above;
    float raise_threshold = 0.0f;
    float clear_threshold = 0.0f;
    uint32_t raise_delay_ms = 0;
    uint32_t clear_delay_ms = 0;
    bool enabled = false;
};

class AlertManager {
public:
    AlertManager();

    bool add_sink(AlertSink &sink);
    bool configure_threshold(const AlertThresholdRule &rule,
                             uint32_t now_ms);

    void set_scope_active(AlertScope scope,
                          bool active,
                          uint32_t now_ms);
    void observe(AlertMetric metric,
                 bool valid,
                 float value,
                 uint32_t now_ms);

private:
    enum class RuleState : uint8_t {
        Idle,
        PendingRaise,
        Active,
        PendingClear,
    };

    struct RuleSlot {
        AlertThresholdRule config;
        RuleState state = RuleState::Idle;
        uint32_t transition_ms = 0;
        uint32_t incident_id = 0;
        float last_value = 0.0f;
        bool occupied = false;
    };

    static constexpr size_t MAX_RULES = 8;
    static constexpr size_t MAX_SINKS = 4;

    static bool state_has_active_incident(RuleState state);
    static bool raise_condition(const AlertThresholdRule &rule,
                                float value);
    static bool clear_condition(const AlertThresholdRule &rule,
                                float value);

    RuleSlot *find_rule(AlertKind kind);
    RuleSlot *find_free_rule();
    void evaluate(RuleSlot &slot,
                  bool valid,
                  float value,
                  uint32_t now_ms);
    void raise(RuleSlot &slot, float value, uint32_t now_ms);
    void clear(RuleSlot &slot,
               AlertClearReason reason,
               uint32_t now_ms);
    void publish(const AlertEvent &event);

    RuleSlot rules_[MAX_RULES];
    AlertSink *sinks_[MAX_SINKS] = {};
    bool scope_active_[static_cast<size_t>(AlertScope::Count)] = {};
    uint32_t next_incident_id_ = 1;
    size_t sink_count_ = 0;
};

const char *alert_kind_name(AlertKind kind);
const char *alert_clear_reason_name(AlertClearReason reason);

}  // namespace aircannect
