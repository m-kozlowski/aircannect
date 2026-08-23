#pragma once

namespace aircannect {

struct MotionSample {
    float x_g = 0.0f;
    float y_g = 0.0f;
    float z_g = 0.0f;
};

class MotionDevice {
public:
    virtual ~MotionDevice() = default;

    virtual bool begin() = 0;
    virtual bool read(MotionSample &sample) = 0;
};

MotionDevice *board_motion_device();

}  // namespace aircannect
