#include "alert_telemetry_adapter.h"

#include "alert_manager.h"
#include "session_manager.h"

namespace aircannect {

void AlertTelemetryAdapter::begin(TherapyTelemetryBroker &telemetry,
                                  AlertManager &alerts,
                                  const SessionManager &session) {
    telemetry_ = &telemetry;
    alerts_ = &alerts;
    session_ = &session;
}

bool AlertTelemetryAdapter::set_leak_enabled(bool enabled) {
    if (leak_enabled_ == enabled) return true;
    if (!telemetry_) return false;

    leak_enabled_ = enabled;
    if (!enabled) {
        telemetry_->release(subscription_);
        subscription_ = THERAPY_TELEMETRY_SUBSCRIPTION_INVALID;
        return true;
    }

    TherapyTelemetryDemand demand;
    demand.metrics = THERAPY_METRIC_LEAK;
    subscription_ = telemetry_->subscribe(demand, *this);
    if (subscription_ != THERAPY_TELEMETRY_SUBSCRIPTION_INVALID) {
        return true;
    }

    leak_enabled_ = false;
    return false;
}

void AlertTelemetryAdapter::poll(uint32_t now_ms) {
    if (!alerts_ || !session_) return;

    alerts_->set_scope_active(
        AlertScope::TherapySession,
        session_->status().state == SessionState::Active,
        now_ms);
}

void AlertTelemetryAdapter::accept_therapy_telemetry(
    const TherapyTelemetrySnapshot &snapshot,
    uint32_t now_ms) {
    if (!alerts_ || !leak_enabled_) return;

    alerts_->observe(AlertMetric::Leak,
                     snapshot.leak_valid,
                     snapshot.leak_l_min,
                     now_ms);
}

}  // namespace aircannect
