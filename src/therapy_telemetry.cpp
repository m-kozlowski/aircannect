#include "therapy_telemetry.h"

namespace aircannect {
namespace {

constexpr uint32_t PRESSURE_FRESH_MS = 5000;

}  // namespace

bool TherapyTelemetry::accept_frame(const StreamFrameData &frame,
                                    uint32_t now_ms) {
    const bool inspiratory_updated = accept_signal(
        frame, StreamSignalId::InspiratoryPressure,
        StreamSignalId::InspiratoryPressureTwoSecond,
        now_ms, inspiratory_pressure_);
    const bool expiratory_updated = accept_signal(
        frame, StreamSignalId::ExpiratoryPressure,
        StreamSignalId::ExpiratoryPressureTwoSecond,
        now_ms, expiratory_pressure_);

    return inspiratory_updated || expiratory_updated;
}

TherapyPressureSnapshot TherapyTelemetry::snapshot(uint32_t now_ms) const {
    TherapyPressureSnapshot out;
    out.inspiratory_pressure_valid = fresh(inspiratory_pressure_, now_ms);
    out.expiratory_pressure_valid = fresh(expiratory_pressure_, now_ms);
    out.inspiratory_pressure = inspiratory_pressure_.value;
    out.expiratory_pressure = expiratory_pressure_.value;
    return out;
}

void TherapyTelemetry::clear() {
    inspiratory_pressure_ = {};
    expiratory_pressure_ = {};
}

bool TherapyTelemetry::accept_signal(const StreamFrameData &frame,
                                     StreamSignalId primary,
                                     StreamSignalId fallback,
                                     uint32_t now_ms,
                                     Reading &reading) {
    float value = 0.0f;
    if (!frame.last_valid_value(primary, value) &&
        !frame.last_valid_value(fallback, value)) {
        return false;
    }

    reading.value = value;
    reading.observed_ms = now_ms;
    reading.valid = true;
    return true;
}

bool TherapyTelemetry::fresh(const Reading &reading, uint32_t now_ms) {
    return reading.valid && now_ms - reading.observed_ms <= PRESSURE_FRESH_MS;
}

}  // namespace aircannect
