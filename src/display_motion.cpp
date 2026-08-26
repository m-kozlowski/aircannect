#include "display_motion.h"

#include <math.h>

namespace aircannect {
namespace {

constexpr float MOTION_DELTA_G = 0.12f;
constexpr float ORIENTATION_AXIS_G = 0.70f;
constexpr float ORIENTATION_DOMINANCE_G = 0.18f;
constexpr uint32_t ORIENTATION_DWELL_MS = 500;
constexpr uint32_t SAMPLE_CONTINUITY_TIMEOUT_MS = 1000;

uint32_t elapsed_ms(uint32_t now_ms, uint32_t since_ms) {
    return now_ms - since_ms;
}

}  // namespace

DisplayMotionUpdate DisplayMotionPolicy::update(
    const MotionSample &sample, uint32_t now_ms) {
    if (have_last_sample_ &&
        elapsed_ms(now_ms, last_sample_ms_) >=
            SAMPLE_CONTINUITY_TIMEOUT_MS) {
        reset_measurement(sample);
    }
    last_sample_ms_ = now_ms;
    have_last_sample_ = true;

    DisplayMotionUpdate result;
    result.wake = motion_detected(sample);
    result.rotation = rotation_;

    const int8_t candidate = orientation_candidate(sample);
    if (candidate < 0 || candidate == rotation_) {
        candidate_rotation_ = -1;
        return result;
    }

    if (candidate != candidate_rotation_) {
        candidate_rotation_ = candidate;
        candidate_since_ms_ = now_ms;
        return result;
    }

    if (elapsed_ms(now_ms, candidate_since_ms_) < ORIENTATION_DWELL_MS) {
        return result;
    }

    rotation_ = static_cast<uint8_t>(candidate);
    candidate_rotation_ = -1;
    result.rotation = rotation_;
    result.rotation_changed = true;
    return result;
}

void DisplayMotionPolicy::set_rotation(uint8_t rotation) {
    rotation_ = rotation & 0x03u;
    candidate_rotation_ = -1;
    have_last_sample_ = false;
}

void DisplayMotionPolicy::reset_measurement(const MotionSample &sample) {
    motion_reference_ = sample;
    have_motion_reference_ = true;
    motion_candidate_ = false;
    candidate_rotation_ = -1;
}

bool DisplayMotionPolicy::motion_detected(const MotionSample &sample) {
    if (!have_motion_reference_) {
        motion_reference_ = sample;
        have_motion_reference_ = true;
        return false;
    }

    const float dx = sample.x_g - motion_reference_.x_g;
    const float dy = sample.y_g - motion_reference_.y_g;
    const float dz = sample.z_g - motion_reference_.z_g;
    const bool moved = dx * dx + dy * dy + dz * dz >=
                       MOTION_DELTA_G * MOTION_DELTA_G;
    if (!moved) {
        motion_candidate_ = false;
        return false;
    }

    if (!motion_candidate_) {
        motion_candidate_ = true;
        return false;
    }

    motion_candidate_ = false;
    motion_reference_ = sample;
    return true;
}

int8_t DisplayMotionPolicy::orientation_candidate(
    const MotionSample &sample) const {
    const float abs_x = fabsf(sample.x_g);
    const float abs_y = fabsf(sample.y_g);
    const float abs_z = fabsf(sample.z_g);

    if (abs_x >= ORIENTATION_AXIS_G &&
        abs_x >= abs_y + ORIENTATION_DOMINANCE_G &&
        abs_x >= abs_z + ORIENTATION_DOMINANCE_G) {
        return sample.x_g > 0.0f ? 3 : 1;
    }

    if (abs_y >= ORIENTATION_AXIS_G &&
        abs_y >= abs_x + ORIENTATION_DOMINANCE_G &&
        abs_y >= abs_z + ORIENTATION_DOMINANCE_G) {
        return sample.y_g > 0.0f ? 0 : 2;
    }

    return -1;
}

}  // namespace aircannect
