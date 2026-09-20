#pragma once

#include "therapy_telemetry_broker.h"

namespace aircannect {

class As11DeviceState;
class As11SettingsState;
class DisplayManager;

class DisplayTelemetryAdapter final : public TherapyTelemetrySubscriber {
public:
    bool begin(TherapyTelemetryBroker &telemetry,
               DisplayManager &display,
               const As11DeviceState &device_state,
               const As11SettingsState &settings);

    void accept_therapy_telemetry(
        const TherapyTelemetrySnapshot &snapshot,
        uint32_t now_ms) override;

private:
    TherapyTelemetryBroker *telemetry_ = nullptr;
    DisplayManager *display_ = nullptr;
    const As11DeviceState *device_state_ = nullptr;
    const As11SettingsState *settings_ = nullptr;
    TherapyTelemetrySubscription subscription_ =
        THERAPY_TELEMETRY_SUBSCRIPTION_INVALID;
};

}  // namespace aircannect
