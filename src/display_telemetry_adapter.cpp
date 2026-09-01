#include "display_telemetry_adapter.h"

#include "as11_device_state.h"
#include "as11_settings.h"
#include "display_manager.h"

namespace aircannect {

bool DisplayTelemetryAdapter::begin(
    TherapyTelemetryBroker &telemetry,
    DisplayManager &display,
    const As11DeviceState &device_state) {
    if (subscription_ != THERAPY_TELEMETRY_SUBSCRIPTION_INVALID) {
        return true;
    }

    telemetry_ = &telemetry;
    display_ = &display;
    device_state_ = &device_state;
    if (!display.available()) return true;

    TherapyTelemetryDemand demand;
    demand.metrics = THERAPY_METRIC_PRESSURE;
    subscription_ = telemetry.subscribe(demand, *this);
    return subscription_ != THERAPY_TELEMETRY_SUBSCRIPTION_INVALID;
}

void DisplayTelemetryAdapter::accept_therapy_telemetry(
    const TherapyTelemetrySnapshot &snapshot,
    uint32_t now_ms) {
    (void)now_ms;
    if (!display_ || !device_state_ || !display_->available()) return;

    const int therapy_mode = as11_mode_index_from_value(
        device_state_->active_therapy_profile());
    display_->publish_therapy_telemetry(snapshot, therapy_mode);
}

}  // namespace aircannect
