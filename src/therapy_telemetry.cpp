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
    const bool leak_updated = accept_signal(
        frame, StreamSignalId::Leak, StreamSignalId::Unknown,
        now_ms, leak_);
    const bool tidal_volume_updated = accept_signal(
        frame, StreamSignalId::TidalVolume, StreamSignalId::Unknown,
        now_ms, tidal_volume_);
    const bool respiratory_rate_updated = accept_signal(
        frame, StreamSignalId::RespiratoryRate, StreamSignalId::Unknown,
        now_ms, respiratory_rate_);
    const bool minute_ventilation_updated = accept_signal(
        frame, StreamSignalId::MinuteVentilation, StreamSignalId::Unknown,
        now_ms, minute_ventilation_);
    const bool flow_limitation_updated = accept_signal(
        frame, StreamSignalId::FlowLimitation, StreamSignalId::Unknown,
        now_ms, flow_limitation_);
    const bool inspiratory_duration_updated = accept_signal(
        frame, StreamSignalId::InspiratoryDuration, StreamSignalId::Unknown,
        now_ms, inspiratory_duration_);
    const bool ie_ratio_updated = accept_signal(
        frame, StreamSignalId::IeRatio, StreamSignalId::Unknown,
        now_ms, ie_ratio_);
    const bool snore_updated = accept_signal(
        frame, StreamSignalId::SnoreIndex, StreamSignalId::Unknown,
        now_ms, snore_);

    return inspiratory_updated || expiratory_updated || leak_updated ||
           tidal_volume_updated || respiratory_rate_updated ||
           minute_ventilation_updated || flow_limitation_updated ||
           inspiratory_duration_updated || ie_ratio_updated ||
           snore_updated;
}

TherapyTelemetrySnapshot TherapyTelemetry::snapshot(uint32_t now_ms) const {
    TherapyTelemetrySnapshot out;
    out.inspiratory_pressure_valid = fresh(inspiratory_pressure_, now_ms);
    out.expiratory_pressure_valid = fresh(expiratory_pressure_, now_ms);
    out.inspiratory_pressure = inspiratory_pressure_.value;
    out.expiratory_pressure = expiratory_pressure_.value;
    out.leak_valid = fresh(leak_, now_ms);
    out.tidal_volume_valid = fresh(tidal_volume_, now_ms);
    out.respiratory_rate_valid = fresh(respiratory_rate_, now_ms);
    out.minute_ventilation_valid = fresh(minute_ventilation_, now_ms);
    out.flow_limitation_valid = fresh(flow_limitation_, now_ms);
    out.inspiratory_duration_valid = fresh(inspiratory_duration_, now_ms);
    out.ie_ratio_valid = fresh(ie_ratio_, now_ms);
    out.snore_valid = fresh(snore_, now_ms);
    out.leak_l_min = leak_.value * 60.0f;
    out.tidal_volume_ml = tidal_volume_.value * 1000.0f;
    out.respiratory_rate = respiratory_rate_.value;
    out.minute_ventilation_l_min = minute_ventilation_.value;
    out.flow_limitation = flow_limitation_.value;
    out.inspiratory_duration_s = inspiratory_duration_.value;
    out.ie_ratio_percent = ie_ratio_.value;
    out.snore = snore_.value;
    return out;
}

void TherapyTelemetry::clear() {
    inspiratory_pressure_ = {};
    expiratory_pressure_ = {};
    leak_ = {};
    tidal_volume_ = {};
    respiratory_rate_ = {};
    minute_ventilation_ = {};
    flow_limitation_ = {};
    inspiratory_duration_ = {};
    ie_ratio_ = {};
    snore_ = {};
}

bool TherapyTelemetry::accept_signal(const StreamFrameData &frame,
                                     StreamSignalId primary,
                                     StreamSignalId fallback,
                                     uint32_t now_ms,
                                     Reading &reading) {
    float value = 0.0f;
    if (!frame.last_valid_value(primary, value) &&
        (fallback == StreamSignalId::Unknown ||
         !frame.last_valid_value(fallback, value))) {
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
