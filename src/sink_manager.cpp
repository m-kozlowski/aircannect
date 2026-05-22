#include "sink_manager.h"

#include <stdio.h>
#include <string.h>

#include "as11_rpc.h"
#include "board.h"

namespace aircannect {
namespace {

const char *acquire_status_name(StreamAcquireStatus status) {
    switch (status) {
        case StreamAcquireStatus::Acquired: return "acquired";
        case StreamAcquireStatus::AlreadyActive: return "already_active";
        case StreamAcquireStatus::Incompatible: return "incompatible";
        case StreamAcquireStatus::Full: return "full";
        case StreamAcquireStatus::Busy: return "busy";
        case StreamAcquireStatus::Rejected:
        default:
            return "rejected";
    }
}

void copy_text(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

}  // namespace

void SinkManager::DebugSink::on_session_start(
    const SessionStatus &session) {
    if (!enabled_) return;
    sessions_started_++;
    last_session_id_ = session.session_id;
}

void SinkManager::DebugSink::on_stream_frame(
    const SessionStatus &session,
    const StreamFrameData &frame) {
    if (!enabled_) return;
    (void)session;
    frames_++;
    if (frame.stream_id) last_stream_id_ = frame.stream_id;
    last_frame_ms_ = millis();
}

void SinkManager::DebugSink::on_session_end(const SessionStatus &session) {
    if (!enabled_) return;
    sessions_ended_++;
    last_session_id_ = session.session_id;
}

void SinkManager::DebugSink::print_status(Print &out) const {
    out.print("[SINK debug] enabled=");
    out.print(enabled_ ? "yes" : "no");
    out.print(" sessions_started=");
    out.print(sessions_started_);
    out.print(" sessions_ended=");
    out.print(sessions_ended_);
    out.print(" frames=");
    out.print(frames_);
    out.print(" last_session=");
    out.print(last_session_id_);
    if (last_stream_id_) {
        out.print(" last_stream=");
        out.print(last_stream_id_);
    }
    if (last_frame_ms_) {
        out.print(" last_frame_age_ms=");
        out.print(millis() - last_frame_ms_);
    }
    out.println();
}

void SinkManager::begin(RpcArbiter &arbiter, SessionManager &session) {
    if (initialized_) return;
    arbiter_ = &arbiter;
    session_ = &session;
    initialized_ = true;
}

void SinkManager::poll() {
    if (!initialized_ || !arbiter_ || !session_) return;
    const uint32_t now = millis();

    dispatch_session_edges();

    if (!debug_sink_.enabled()) {
        release_debug_stream();
        debug_sink_.poll();
        return;
    }

    attach_debug_stream(now);
    drain_debug_stream(now);
    debug_sink_.poll();
}

void SinkManager::set_debug_enabled(bool enabled) {
    const bool was_enabled = debug_sink_.enabled();
    debug_sink_.set_enabled(enabled);
    status_.debug_enabled = enabled;
    if (!enabled) {
        release_debug_stream();
        return;
    }
    if (was_enabled || !session_) return;

    const SessionStatus &session = session_->status();
    seen_session_starts_ = session.start_count;
    seen_session_ends_ = session.end_count;
    if (session.state == SessionState::Active) {
        debug_sink_.on_session_start(session);
        status_.sessions_started++;
    }
}

bool SinkManager::debug_enabled() const {
    return debug_sink_.enabled();
}

void SinkManager::print_status(Print &out) const {
    out.print("[SINK] debug=");
    out.print(debug_sink_.enabled() ? "on" : "off");
    out.print(" stream=");
    out.print(status_.debug_stream_attached ? "attached" : "detached");
    out.print(" handle=");
    out.print(status_.debug_stream_handle);
    out.print(" attach_attempts=");
    out.print(status_.attach_attempts);
    out.print(" attach_failures=");
    out.print(status_.attach_failures);
    out.print(" frames=");
    out.print(status_.frames);
    out.print(" drops=");
    out.print(status_.frame_drops);
    out.print(" sessions_started=");
    out.print(status_.sessions_started);
    out.print(" sessions_ended=");
    out.print(status_.sessions_ended);
    if (status_.last_frame_ms) {
        out.print(" last_frame_age_ms=");
        out.print(millis() - status_.last_frame_ms);
    }
    if (status_.last_error[0]) {
        out.print(" error=");
        out.print(status_.last_error);
    }
    out.println();
    debug_sink_.print_status(out);
}

void SinkManager::dispatch_session_edges() {
    const SessionStatus &session = session_->status();
    if (session.start_count != seen_session_starts_) {
        seen_session_starts_ = session.start_count;
        if (debug_sink_.enabled()) {
            debug_sink_.on_session_start(session);
            status_.sessions_started++;
        }
    }
    if (session.end_count != seen_session_ends_) {
        seen_session_ends_ = session.end_count;
        if (debug_sink_.enabled()) {
            debug_sink_.on_session_end(session);
            status_.sessions_ended++;
        }
    }
}

void SinkManager::attach_debug_stream(uint32_t now_ms) {
    if (status_.debug_stream_handle != STREAM_CONSUMER_INVALID &&
        arbiter_->stream_consumer_active(status_.debug_stream_handle)) {
        status_.debug_stream_attached = true;
        return;
    }
    status_.debug_stream_handle = STREAM_CONSUMER_INVALID;
    status_.debug_stream_attached = false;
    if (static_cast<int32_t>(now_ms - next_attach_ms_) < 0) return;

    next_attach_ms_ = now_ms + AC_SINK_ATTACH_RETRY_MS;
    status_.attach_attempts++;
    const std::string params =
        build_stream_params(DEFAULT_EDF_STREAM_IDS, 10, 50);
    StreamAcquireResult result =
        arbiter_->acquire_stream(params, RpcSource::Sink);
    if (result.status == StreamAcquireStatus::Acquired ||
        result.status == StreamAcquireStatus::AlreadyActive) {
        status_.debug_stream_handle = result.handle;
        status_.debug_stream_attached = true;
        last_debug_queue_drops_ = 0;
        status_.last_error[0] = 0;
        return;
    }

    status_.attach_failures++;
    set_error(acquire_status_name(result.status));
}

void SinkManager::release_debug_stream() {
    if (!arbiter_) return;
    if (status_.debug_stream_handle != STREAM_CONSUMER_INVALID &&
        arbiter_->stream_consumer_active(status_.debug_stream_handle)) {
        arbiter_->release_stream(status_.debug_stream_handle);
    }
    status_.debug_stream_handle = STREAM_CONSUMER_INVALID;
    status_.debug_stream_attached = false;
    last_debug_queue_drops_ = 0;
}

void SinkManager::drain_debug_stream(uint32_t now_ms) {
    if (status_.debug_stream_handle == STREAM_CONSUMER_INVALID ||
        !arbiter_->stream_consumer_active(status_.debug_stream_handle)) {
        status_.debug_stream_attached = false;
        return;
    }

    const uint32_t queue_drops =
        arbiter_->stream_consumer_queue_drops(status_.debug_stream_handle);
    if (queue_drops < last_debug_queue_drops_) {
        last_debug_queue_drops_ = queue_drops;
    } else if (queue_drops != last_debug_queue_drops_) {
        const uint32_t delta = queue_drops - last_debug_queue_drops_;
        last_debug_queue_drops_ = queue_drops;
        status_.frame_drops += delta;
        session_->note_frame_drops(delta, now_ms);
    }

    for (size_t i = 0; i < AC_SINK_STREAM_FRAME_BUDGET; ++i) {
        StreamFrameRef frame;
        if (!arbiter_->next_stream_frame(status_.debug_stream_handle,
                                         frame)) {
            break;
        }
        if (!frame) continue;
        debug_sink_.on_stream_frame(session_->status(), *frame);
        status_.frames++;
        status_.last_frame_ms = now_ms;
    }
}

void SinkManager::set_error(const char *error) {
    copy_text(status_.last_error, sizeof(status_.last_error), error);
}

}  // namespace aircannect
