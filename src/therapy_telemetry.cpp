#include "therapy_telemetry.h"

namespace aircannect {
namespace {

constexpr uint32_t PRESSURE_FRESH_MS = 5000;

}  // namespace

void TherapyTelemetry::accept_frame(const StreamFrameData &frame,
                                    uint32_t now_ms) {
    accept_signal(frame, StreamSignalId::MaskPressure,
                  StreamSignalId::MaskPressureTwoSecond,
                  now_ms, mask_pressure_);
    accept_signal(frame, StreamSignalId::InspiratoryPressure,
                  StreamSignalId::InspiratoryPressureTwoSecond,
                  now_ms, inspiratory_pressure_);
    accept_signal(frame, StreamSignalId::ExpiratoryPressure,
                  StreamSignalId::ExpiratoryPressureTwoSecond,
                  now_ms, expiratory_pressure_);
}

TherapyPressureSnapshot TherapyTelemetry::snapshot(uint32_t now_ms) const {
    TherapyPressureSnapshot out;
    out.mask_pressure_valid = fresh(mask_pressure_, now_ms);
    out.inspiratory_pressure_valid = fresh(inspiratory_pressure_, now_ms);
    out.expiratory_pressure_valid = fresh(expiratory_pressure_, now_ms);
    out.mask_pressure = mask_pressure_.value;
    out.inspiratory_pressure = inspiratory_pressure_.value;
    out.expiratory_pressure = expiratory_pressure_.value;
    return out;
}

void TherapyTelemetry::clear() {
    mask_pressure_ = {};
    inspiratory_pressure_ = {};
    expiratory_pressure_ = {};
}

void TherapyTelemetry::accept_signal(const StreamFrameData &frame,
                                     StreamSignalId primary,
                                     StreamSignalId fallback,
                                     uint32_t now_ms,
                                     Reading &reading) {
    float value = 0.0f;
    if (!frame.last_valid_value(primary, value) &&
        !frame.last_valid_value(fallback, value)) {
        return;
    }

    reading.value = value;
    reading.observed_ms = now_ms;
    reading.valid = true;
}

bool TherapyTelemetry::fresh(const Reading &reading, uint32_t now_ms) {
    return reading.valid && now_ms - reading.observed_ms <= PRESSURE_FRESH_MS;
}

}  // namespace aircannect
