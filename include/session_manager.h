#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "as11_device_state.h"
#include "stream_frame.h"

namespace aircannect {

enum class SessionState {
    Idle,
    Active,
};

struct SessionStatus {
    SessionState state = SessionState::Idle;
    As11TherapyState therapy_state = As11TherapyState::Unknown;
    uint32_t session_id = 0;
    uint32_t start_count = 0;
    uint32_t end_count = 0;
    uint32_t started_ms = 0;
    uint32_t ended_ms = 0;
    uint32_t last_frame_ms = 0;
    uint32_t frame_count = 0;
    uint32_t dropped_frames = 0;
    uint32_t stream_id = 0;
    char start_device_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
    char end_device_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
    char last_stream_start_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
    char end_reason[32] = {};
};

class SessionManager {
public:
    void begin();
    void poll(const As11DeviceState &as11, uint32_t now_ms);

    void note_stream_frame(const StreamFrameData &frame, uint32_t now_ms);
    void note_frame_drops(uint32_t drops, uint32_t now_ms);
    void note_device_boot(uint32_t now_ms);

    const SessionStatus &status() const { return status_; }
    static const char *state_name(SessionState state);

private:
    void start_session(const As11DeviceState &as11,
                       uint32_t now_ms,
                       const char *reason);
    void end_session(const As11DeviceState &as11,
                     uint32_t now_ms,
                     const char *reason);
    void copy_time(char *dst, size_t size, const std::string &value);
    void copy_text(char *dst, size_t size, const char *value);

    SessionStatus status_;
    uint32_t next_session_id_ = 0;
    bool initialized_ = false;
};

}  // namespace aircannect
