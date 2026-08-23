#pragma once

#include <stdint.h>

#include "stream_frame.h"

namespace aircannect {

struct TherapyPressureSnapshot {
    bool mask_pressure_valid = false;
    bool inspiratory_pressure_valid = false;
    bool expiratory_pressure_valid = false;
    float mask_pressure = 0.0f;
    float inspiratory_pressure = 0.0f;
    float expiratory_pressure = 0.0f;
};

class TherapyTelemetry {
public:
    void accept_frame(const StreamFrameData &frame, uint32_t now_ms);
    TherapyPressureSnapshot snapshot(uint32_t now_ms) const;
    void clear();

private:
    struct Reading {
        float value = 0.0f;
        uint32_t observed_ms = 0;
        bool valid = false;
    };

    static void accept_signal(const StreamFrameData &frame,
                              StreamSignalId primary,
                              StreamSignalId fallback,
                              uint32_t now_ms,
                              Reading &reading);
    static bool fresh(const Reading &reading, uint32_t now_ms);

    Reading mask_pressure_;
    Reading inspiratory_pressure_;
    Reading expiratory_pressure_;
};

}  // namespace aircannect
