#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

struct ExportTaskControlSnapshot;
struct SessionStatus;
struct SystemStatusSnapshot;
struct TherapyTelemetrySnapshot;

static constexpr size_t DISPLAY_WIFI_VALUE_MAX = 33;

enum class DisplayAir11State : uint8_t {
    Connecting,
    Ready,
    Unavailable,
};

enum class DisplayWifiState : uint8_t {
    Connecting,
    Sta,
    SoftAp,
    Offline,
};

enum class DisplayExportState : uint8_t {
    Disabled,
    Ready,
    Pending,
    Working,
    Error,
};

struct DisplayPressureSnapshot {
    enum class Kind : uint8_t {
        Unavailable,
        Single,
        Pair,
    };

    Kind kind = Kind::Unavailable;
    float inspiratory = 0.0f;
    float expiratory = 0.0f;
};

struct DisplaySnapshot {
    uint32_t generation = 0;
    char local_time[6] = "--:--";
    bool therapy_active = false;
    uint32_t therapy_elapsed_s = 0;

    DisplayAir11State air11 = DisplayAir11State::Connecting;
    DisplayWifiState wifi = DisplayWifiState::Offline;
    char wifi_value[DISPLAY_WIFI_VALUE_MAX] = {};
    DisplayExportState smb = DisplayExportState::Disabled;
    DisplayExportState sleephq = DisplayExportState::Disabled;

    DisplayPressureSnapshot pressure;

    bool leak_valid = false;
    bool tidal_volume_valid = false;
    bool respiratory_rate_valid = false;
    bool minute_ventilation_valid = false;
    bool flow_limitation_valid = false;
    bool inspiratory_duration_valid = false;
    bool ie_ratio_valid = false;
    bool snore_valid = false;
    float leak_l_min = 0.0f;
    float tidal_volume_ml = 0.0f;
    float respiratory_rate = 0.0f;
    float minute_ventilation_l_min = 0.0f;
    float flow_limitation = 0.0f;
    float inspiratory_duration_s = 0.0f;
    float ie_ratio_percent = 0.0f;
    float snore = 0.0f;

    bool oximetry_valid = false;
    int16_t spo2 = -1;
    int16_t pulse_bpm = -1;
};

DisplayPressureSnapshot compose_display_pressure(
    const TherapyTelemetrySnapshot &telemetry,
    int therapy_mode);
void apply_display_therapy_telemetry(
    DisplaySnapshot &snapshot,
    const TherapyTelemetrySnapshot &telemetry,
    int therapy_mode);

DisplaySnapshot compose_display_snapshot(
    const SystemStatusSnapshot &system,
    const SessionStatus &session,
    const TherapyTelemetrySnapshot &telemetry,
    const ExportTaskControlSnapshot &exports,
    int therapy_mode);

}  // namespace aircannect
