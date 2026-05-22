#include "session_manager.h"

#include <stdio.h>
#include <string.h>

#include "debug_log.h"

namespace aircannect {

void SessionManager::begin() {
    if (initialized_) return;
    initialized_ = true;
}

void SessionManager::poll(const As11DeviceState &as11, uint32_t now_ms) {
    if (!initialized_) begin();

    status_.therapy_state = as11.therapy_state();
    if (as11.therapy_state() == As11TherapyState::Running) {
        if (status_.state != SessionState::Active) {
            start_session(as11, now_ms, "rop_running");
        }
        return;
    }

    if (status_.state == SessionState::Active &&
        as11.therapy_state() == As11TherapyState::Standby) {
        end_session(as11, now_ms, "rop_standby");
    }
}

void SessionManager::note_stream_frame(const StreamFrameData &frame,
                                       uint32_t now_ms) {
    if (status_.state != SessionState::Active) return;
    status_.frame_count++;
    status_.last_frame_ms = now_ms;
    if (frame.stream_id) status_.stream_id = frame.stream_id;
    if (frame.start_time[0]) {
        copy_text(status_.last_stream_start_time,
                  sizeof(status_.last_stream_start_time),
                  frame.start_time);
    }
}

void SessionManager::note_frame_drops(uint32_t drops, uint32_t now_ms) {
    if (status_.state != SessionState::Active || drops == 0) return;
    status_.dropped_frames += drops;
    status_.last_frame_ms = now_ms;
}

void SessionManager::note_device_boot(uint32_t now_ms) {
    if (status_.state != SessionState::Active) return;
    As11DeviceState empty;
    end_session(empty, now_ms, "device_boot");
}

const char *SessionManager::state_name(SessionState state) {
    switch (state) {
        case SessionState::Active: return "active";
        case SessionState::Idle:
        default:
            return "idle";
    }
}

void SessionManager::start_session(const As11DeviceState &as11,
                                   uint32_t now_ms,
                                   const char *reason) {
    const uint32_t start_count = status_.start_count;
    const uint32_t end_count = status_.end_count;
    status_ = SessionStatus();
    status_.state = SessionState::Active;
    status_.therapy_state = as11.therapy_state();
    status_.session_id = ++next_session_id_;
    status_.start_count = start_count + 1;
    status_.end_count = end_count;
    status_.started_ms = now_ms;
    copy_time(status_.start_device_time,
              sizeof(status_.start_device_time),
              as11.device_datetime());
    Log::logf(CAT_STREAM, LOG_INFO,
              "[SESSION] started id=%lu reason=%s device_time=%s\n",
              static_cast<unsigned long>(status_.session_id),
              reason ? reason : "--",
              status_.start_device_time[0] ? status_.start_device_time : "--");
}

void SessionManager::end_session(const As11DeviceState &as11,
                                 uint32_t now_ms,
                                 const char *reason) {
    status_.state = SessionState::Idle;
    status_.therapy_state = as11.therapy_state();
    status_.ended_ms = now_ms;
    status_.end_count++;
    copy_time(status_.end_device_time,
              sizeof(status_.end_device_time),
              as11.device_datetime());
    copy_text(status_.end_reason, sizeof(status_.end_reason), reason);
    Log::logf(CAT_STREAM, LOG_INFO,
              "[SESSION] ended id=%lu reason=%s frames=%lu drops=%lu\n",
              static_cast<unsigned long>(status_.session_id),
              status_.end_reason[0] ? status_.end_reason : "--",
              static_cast<unsigned long>(status_.frame_count),
              static_cast<unsigned long>(status_.dropped_frames));
}

void SessionManager::copy_time(char *dst,
                               size_t size,
                               const std::string &value) {
    copy_text(dst, size, value.c_str());
}

void SessionManager::copy_text(char *dst, size_t size, const char *value) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", value ? value : "");
}

}  // namespace aircannect
