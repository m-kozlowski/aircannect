#pragma once

#include <string>
#include <stdint.h>

#include "resmed_device_model.h"

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
    TriggerCycleEvent,
};

StreamSignalId as11_stream_signal_id_from_name(
    const char *name,
    ResmedDeviceModel model = ResmedDeviceModel::Unknown);
uint32_t as11_stream_signal_sample_interval_ms(
    const char *name,
    uint32_t fallback_interval_ms,
    ResmedDeviceModel model = ResmedDeviceModel::Unknown);

const char *as11_stream_signal_wire_name(
    const char *canonical_name,
    ResmedDeviceModel model);
const char *as11_stream_signal_canonical_name(
    const char *wire_name,
    ResmedDeviceModel model);
bool as11_stream_signal_wire_ids(const std::string &canonical_ids_csv,
                                 ResmedDeviceModel model,
                                 std::string &wire_ids_csv);

}  // namespace aircannect
