#include "sink_manager.h"

#include "as11_rpc.h"
#include "board.h"
#include "memory_manager.h"
#include "string_util.h"

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

void SinkManager::begin(RpcArbiter &arbiter,
                        const As11DeviceState &device_state,
                        SessionManager &session) {
    if (initialized_) return;
    arbiter_ = &arbiter;
    device_state_ = &device_state;
    session_ = &session;
    initialized_ = true;
}

void SinkManager::poll() {
    if (!initialized_ || !arbiter_ || !session_) return;
    const uint32_t now = millis();

    poll_live_chart(now);
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

bool SinkManager::live_chart_should_run() const {
    if (!live_chart_.enabled || !arbiter_ || !device_state_ || !session_) {
        return false;
    }
    if (device_state_->therapy_state() == As11TherapyState::Running) {
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

void SinkManager::set_live_error(const char *error) {
    copy_cstr(live_chart_.last_error, sizeof(live_chart_.last_error), error);
}

}  // namespace aircannect
