#pragma once

#include "therapy_telemetry_broker.h"

namespace aircannect {

class AlertManager;
class SessionManager;

class AlertTelemetryAdapter final : public TherapyTelemetrySubscriber {
public:
    void begin(TherapyTelemetryBroker &telemetry,
               AlertManager &alerts,
               const SessionManager &session);
    bool set_leak_enabled(bool enabled);
    void poll(uint32_t now_ms);

    void accept_therapy_telemetry(
        const TherapyTelemetrySnapshot &snapshot,
        uint32_t now_ms) override;

private:
    TherapyTelemetryBroker *telemetry_ = nullptr;
    AlertManager *alerts_ = nullptr;
    const SessionManager *session_ = nullptr;
    TherapyTelemetrySubscription subscription_ =
        THERAPY_TELEMETRY_SUBSCRIPTION_INVALID;
    bool leak_enabled_ = false;
};

}  // namespace aircannect
