#pragma once

#include <stdint.h>

#include "motion_device.h"

namespace aircannect {

struct DisplayMotionUpdate {
    bool wake = false;
    bool rotation_changed = false;
    uint8_t rotation = 0;
};

class DisplayMotionPolicy {
public:
    DisplayMotionUpdate update(const MotionSample &sample, uint32_t now_ms);

    uint8_t rotation() const { return rotation_; }

private:
    bool motion_detected(const MotionSample &sample);
    int8_t orientation_candidate(const MotionSample &sample) const;

    MotionSample motion_reference_;
    bool have_motion_reference_ = false;
    bool motion_candidate_ = false;

    int8_t candidate_rotation_ = -1;
    uint32_t candidate_since_ms_ = 0;
    uint8_t rotation_ = 0;
};

}  // namespace aircannect
