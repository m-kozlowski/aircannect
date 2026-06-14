#pragma once

#include <stdint.h>

namespace aircannect {

enum class StreamSignalId : uint8_t {
    Unknown,
    PatientFlow,
    MaskPressure,
    MaskPressureTwoSecond,
    InspiratoryPressure,
    ExpiratoryPressure,
    InspiratoryPressureTwoSecond,
    ExpiratoryPressureTwoSecond,
    Leak,
    RespiratoryRate,
    TidalVolume,
    MinuteVentilation,
    TargetMinuteVentilation,
    IeRatio,
    SnoreIndex,
    FlowLimitation,
    InspiratoryDuration,
    HeartRate,
    SpO2,
};

StreamSignalId as11_stream_signal_id_from_name(const char *name);
const char *as11_stream_signal_id_name(StreamSignalId id);
uint32_t as11_stream_signal_sample_interval_ms(
    const char *name,
    uint32_t fallback_interval_ms);

}  // namespace aircannect
