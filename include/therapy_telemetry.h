#pragma once

#include <stdint.h>

#include "stream_frame.h"

namespace aircannect {

struct TherapyTelemetrySnapshot {
    bool inspiratory_pressure_valid = false;
    bool expiratory_pressure_valid = false;
    float inspiratory_pressure = 0.0f;
    float expiratory_pressure = 0.0f;

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
};

class TherapyTelemetry {
public:
    bool accept_frame(const StreamFrameData &frame, uint32_t now_ms);
    TherapyTelemetrySnapshot snapshot(uint32_t now_ms) const;
    void clear();

private:
    struct Reading {
        float value = 0.0f;
        uint32_t observed_ms = 0;
        bool valid = false;
    };

    static bool accept_signal(const StreamFrameData &frame,
                              StreamSignalId primary,
                              StreamSignalId fallback,
                              uint32_t now_ms,
                              Reading &reading);
    static bool fresh(const Reading &reading, uint32_t now_ms);

    Reading inspiratory_pressure_;
    Reading expiratory_pressure_;
    Reading leak_;
    Reading tidal_volume_;
    Reading respiratory_rate_;
    Reading minute_ventilation_;
    Reading flow_limitation_;
    Reading inspiratory_duration_;
    Reading ie_ratio_;
    Reading snore_;
};

}  // namespace aircannect
