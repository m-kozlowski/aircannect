#include "sink_manager.h"

#include <stdio.h>
#include <string.h>

#include "as11_rpc.h"
#include "board.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

const char *const LIVE_CHART_STREAM_IDS =
    "_RFL,"
    "_MKP,"
    "_LKF,"
    "_MKI,"
    "_MKE,"
    "_HRT,"
    "_SAO";

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

void append_live_sample(LiveChartSeriesBatch &series,
                        bool valid,
                        float value,
                        uint32_t &drops) {
    if (!series.values || !series.valid ||
        series.count >= series.capacity) {
        drops++;
        return;
    }
    const size_t index = series.count++;
    series.valid[index] = valid ? 1 : 0;
    series.values[index] = valid ? value : 0.0f;
}

void clear_live_series(LiveChartSeriesBatch &series) {
    series.count = 0;
}

void release_live_series(LiveChartSeriesBatch &series) {
    Memory::free(series.values);
    Memory::free(series.valid);
    series.values = nullptr;
    series.valid = nullptr;
    series.capacity = 0;
    series.count = 0;
}

bool ensure_live_series(LiveChartSeriesBatch &series) {
    if (series.values && series.valid &&
        series.capacity >= AC_WEB_LIVE_BATCH_SAMPLES_MAX) {
        return true;
    }
    release_live_series(series);
    series.values = static_cast<float *>(
        Memory::alloc_large(sizeof(float) * AC_WEB_LIVE_BATCH_SAMPLES_MAX));
    series.valid = static_cast<uint8_t *>(
        Memory::alloc_large(AC_WEB_LIVE_BATCH_SAMPLES_MAX));
    if (!series.values || !series.valid) {
        release_live_series(series);
        return false;
    }
    series.capacity = AC_WEB_LIVE_BATCH_SAMPLES_MAX;
    return true;
}

bool append_frame_signal(const StreamFrameData &frame,
                         StreamSignalId id,
                         LiveChartSeriesBatch &series,
                         uint32_t &drops,
                         float scale = 1.0f) {
    const StreamSignalSpan *span = frame.find_signal(id);
    if (!span) return false;
    for (uint16_t i = 0; i < span->sample_count; ++i) {
        const size_t index = span->value_offset + i;
        const bool valid =
            index < frame.value_count && frame.value_valid(index);
        append_live_sample(series, valid,
                           valid ? frame.values[index] * scale : 0.0f,
                           drops);
    }
    return true;
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

DebugSinkRuntimeStatus SinkManager::DebugSink::status() const {
    DebugSinkRuntimeStatus out;
    out.enabled = enabled_;
    out.sessions_started = sessions_started_;
    out.sessions_ended = sessions_ended_;
    out.frames = frames_;
    out.last_session_id = last_session_id_;
    out.last_stream_id = last_stream_id_;
    out.last_frame_ms = last_frame_ms_;
    return out;
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
    poll_live_chart(now);

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

DebugSinkRuntimeStatus SinkManager::debug_status() const {
    return debug_sink_.status();
}

void SinkManager::set_live_chart_enabled(bool enabled) {
    if (live_chart_.enabled == enabled) return;
    live_chart_.enabled = enabled;
    live_chart_.state_dirty = true;
    if (!enabled) {
        release_live_chart_stream();
        release_live_chart_batches();
    }
}

bool SinkManager::live_chart_enabled() const {
    return live_chart_.enabled;
}

void SinkManager::clear_live_chart_batch() {
    clear_live_series(live_chart_.pressure);
    clear_live_series(live_chart_.flow);
    clear_live_series(live_chart_.leak);
    clear_live_series(live_chart_.inspiratory_pressure);
    clear_live_series(live_chart_.expiratory_pressure);
    clear_live_series(live_chart_.spo2);
    clear_live_series(live_chart_.pulse);
}

void SinkManager::mark_live_chart_sent() {
    live_chart_.state_dirty = false;
    clear_live_chart_batch();
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
        build_stream_params(DEFAULT_EDF_STREAM_IDS, 40, 200);
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

bool SinkManager::live_chart_should_run() const {
    if (!live_chart_.enabled || !arbiter_ || !session_) return false;
    if (arbiter_->as11_state().therapy_state() == As11TherapyState::Running) {
        return true;
    }
    return session_->status().state == SessionState::Active;
}

void SinkManager::poll_live_chart(uint32_t now_ms) {
    const bool should_run = live_chart_should_run();
    if (live_chart_.desired != should_run) {
        live_chart_.desired = should_run;
        live_chart_.state_dirty = true;
    }
    if (!should_run) {
        release_live_chart_stream();
        release_live_chart_batches();
        return;
    }
    if (live_chart_.pressure.capacity == 0 &&
        static_cast<int32_t>(now_ms - next_live_attach_ms_) < 0) {
        return;
    }
    if (!ensure_live_chart_batches()) {
        next_live_attach_ms_ = now_ms + AC_SINK_ATTACH_RETRY_MS;
        live_chart_.attach_failures++;
        live_chart_.state_dirty = true;
        release_live_chart_stream();
        set_live_error("live_batch_alloc_failed");
        return;
    }

    attach_live_chart_stream(now_ms);
    drain_live_chart_stream(now_ms);
}

bool SinkManager::ensure_live_chart_batches() {
    const bool ok =
        ensure_live_series(live_chart_.pressure) &&
        ensure_live_series(live_chart_.flow) &&
        ensure_live_series(live_chart_.leak) &&
        ensure_live_series(live_chart_.inspiratory_pressure) &&
        ensure_live_series(live_chart_.expiratory_pressure) &&
        ensure_live_series(live_chart_.spo2) &&
        ensure_live_series(live_chart_.pulse);
    if (!ok) release_live_chart_batches();
    return ok;
}

void SinkManager::release_live_chart_batches() {
    release_live_series(live_chart_.pressure);
    release_live_series(live_chart_.flow);
    release_live_series(live_chart_.leak);
    release_live_series(live_chart_.inspiratory_pressure);
    release_live_series(live_chart_.expiratory_pressure);
    release_live_series(live_chart_.spo2);
    release_live_series(live_chart_.pulse);
}

void SinkManager::attach_live_chart_stream(uint32_t now_ms) {
    if (live_chart_.handle != STREAM_CONSUMER_INVALID &&
        arbiter_->stream_consumer_active(live_chart_.handle)) {
        if (!live_chart_.attached) {
            live_chart_.attached = true;
            live_chart_.state_dirty = true;
        }
        return;
    }
    live_chart_.handle = STREAM_CONSUMER_INVALID;
    if (live_chart_.attached) {
        live_chart_.attached = false;
        live_chart_.state_dirty = true;
    }
    if (static_cast<int32_t>(now_ms - next_live_attach_ms_) < 0) return;

    next_live_attach_ms_ = now_ms + AC_SINK_ATTACH_RETRY_MS;
    const std::string params =
        build_stream_params(LIVE_CHART_STREAM_IDS, 40, 200);
    StreamAcquireResult result =
        arbiter_->acquire_stream(params, RpcSource::Sink);
    if (result.status == StreamAcquireStatus::Acquired ||
        result.status == StreamAcquireStatus::AlreadyActive) {
        live_chart_.handle = result.handle;
        live_chart_.attached = true;
        live_chart_.state_dirty = true;
        last_live_queue_drops_ = 0;
        live_chart_.last_error[0] = 0;
        return;
    }

    live_chart_.attach_failures++;
    live_chart_.state_dirty = true;
    set_live_error(acquire_status_name(result.status));
}

void SinkManager::release_live_chart_stream() {
    if (!arbiter_) return;
    if (live_chart_.handle != STREAM_CONSUMER_INVALID &&
        arbiter_->stream_consumer_active(live_chart_.handle)) {
        arbiter_->release_stream(live_chart_.handle);
    }
    const bool was_attached =
        live_chart_.handle != STREAM_CONSUMER_INVALID || live_chart_.attached;
    live_chart_.handle = STREAM_CONSUMER_INVALID;
    live_chart_.attached = false;
    last_live_queue_drops_ = 0;
    if (was_attached) live_chart_.state_dirty = true;
}

void SinkManager::drain_live_chart_stream(uint32_t now_ms) {
    if (live_chart_.handle == STREAM_CONSUMER_INVALID ||
        !arbiter_->stream_consumer_active(live_chart_.handle)) {
        if (live_chart_.attached) live_chart_.state_dirty = true;
        live_chart_.attached = false;
        return;
    }

    const uint32_t queue_drops =
        arbiter_->stream_consumer_queue_drops(live_chart_.handle);
    if (queue_drops < last_live_queue_drops_) {
        last_live_queue_drops_ = queue_drops;
    } else if (queue_drops != last_live_queue_drops_) {
        const uint32_t delta = queue_drops - last_live_queue_drops_;
        last_live_queue_drops_ = queue_drops;
        live_chart_.drops += delta;
    }

    for (size_t i = 0; i < AC_WEB_LIVE_FRAME_BUDGET; ++i) {
        StreamFrameRef frame;
        if (!arbiter_->next_stream_frame(live_chart_.handle, frame)) break;
        if (!frame) continue;

        append_frame_signal(*frame, StreamSignalId::MaskPressure,
                            live_chart_.pressure, live_chart_.drops);
        append_frame_signal(*frame, StreamSignalId::PatientFlow,
                            live_chart_.flow, live_chart_.drops, 60.0f);
        append_frame_signal(*frame, StreamSignalId::Leak,
                            live_chart_.leak, live_chart_.drops, 60.0f);
        if (!append_frame_signal(*frame,
                                 StreamSignalId::InspiratoryPressure,
                                 live_chart_.inspiratory_pressure,
                                 live_chart_.drops)) {
            append_frame_signal(*frame,
                                StreamSignalId::InspiratoryPressureTwoSecond,
                                live_chart_.inspiratory_pressure,
                                live_chart_.drops);
        }
        if (!append_frame_signal(*frame,
                                 StreamSignalId::ExpiratoryPressure,
                                 live_chart_.expiratory_pressure,
                                 live_chart_.drops)) {
            append_frame_signal(*frame,
                                StreamSignalId::ExpiratoryPressureTwoSecond,
                                live_chart_.expiratory_pressure,
                                live_chart_.drops);
        }
        append_frame_signal(*frame, StreamSignalId::SpO2,
                            live_chart_.spo2, live_chart_.drops);
        append_frame_signal(*frame, StreamSignalId::HeartRate,
                            live_chart_.pulse, live_chart_.drops);

        live_chart_.frames++;
        live_chart_.last_frame_ms = now_ms;
    }
}

void SinkManager::set_error(const char *error) {
    copy_text(status_.last_error, sizeof(status_.last_error), error);
}

void SinkManager::set_live_error(const char *error) {
    copy_text(live_chart_.last_error, sizeof(live_chart_.last_error), error);
}

}  // namespace aircannect
