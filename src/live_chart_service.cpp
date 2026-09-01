#include "live_chart_service.h"

#include "as11_rpc.h"
#include "board.h"
#include "memory_manager.h"
#include "oximetry_hub.h"
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

constexpr uint32_t LIVE_CHART_ATTACH_RETRY_MS = 2000;

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

void LiveChartService::begin(StreamBroker &stream,
                             const As11DeviceState &device_state,
                             SessionManager &session) {
    if (initialized_) return;
    stream_ = &stream;
    device_state_ = &device_state;
    session_ = &session;
    initialized_ = true;
}

void LiveChartService::poll(uint32_t now_ms) {
    if (!initialized_ || !stream_ || !session_) return;

    const bool desired = should_run();
    if (status_.desired != desired) {
        status_.desired = desired;
        status_.state_dirty = true;
    }
    if (!desired) {
        release_stream();
        release_batches();
        return;
    }
    if (status_.pressure.capacity == 0 &&
        static_cast<int32_t>(now_ms - next_attach_ms_) < 0) {
        return;
    }
    if (!ensure_batches()) {
        next_attach_ms_ = now_ms + LIVE_CHART_ATTACH_RETRY_MS;
        status_.attach_failures++;
        status_.state_dirty = true;
        release_stream();
        set_error("live_batch_alloc_failed");
        return;
    }

    attach_stream(now_ms);
    drain_stream(now_ms);
}

void LiveChartService::set_enabled(bool enabled) {
    if (status_.enabled == enabled) return;

    status_.enabled = enabled;
    status_.state_dirty = true;
    if (!enabled) {
        release_stream();
        release_batches();
    }
}

void LiveChartService::update_local_oximetry(
    const OximetryHubSnapshot &source,
    bool mirrored_to_as11) {
    local_oximetry_direct_ = source.source_present && !mirrored_to_as11;
    if (!source.source_present) {
        last_local_oximetry_ms_ = 0;
        return;
    }

    const OximetryReading &reading = source.reading;
    if (!reading.timestamp_ms ||
        reading.timestamp_ms == last_local_oximetry_ms_) {
        return;
    }
    last_local_oximetry_ms_ = reading.timestamp_ms;

    if (!local_oximetry_direct_ || !initialized_ ||
        !status_.enabled || !status_.desired) {
        return;
    }

    append_live_sample(status_.spo2, reading.valid,
                       static_cast<float>(reading.spo2), status_.drops);
    append_live_sample(status_.pulse, reading.valid,
                       static_cast<float>(reading.pulse_bpm),
                       status_.drops);
}

void LiveChartService::clear_batch() {
    clear_live_series(status_.pressure);
    clear_live_series(status_.flow);
    clear_live_series(status_.leak);
    clear_live_series(status_.inspiratory_pressure);
    clear_live_series(status_.expiratory_pressure);
    clear_live_series(status_.spo2);
    clear_live_series(status_.pulse);
}

void LiveChartService::mark_sent() {
    status_.state_dirty = false;
    clear_batch();
}

bool LiveChartService::should_run() const {
    if (!status_.enabled || !stream_ || !device_state_ || !session_) {
        return false;
    }
    if (device_state_->therapy_state() == As11TherapyState::Running) {
        return true;
    }
    return session_->status().state == SessionState::Active;
}

bool LiveChartService::ensure_batches() {
    const bool ok =
        ensure_live_series(status_.pressure) &&
        ensure_live_series(status_.flow) &&
        ensure_live_series(status_.leak) &&
        ensure_live_series(status_.inspiratory_pressure) &&
        ensure_live_series(status_.expiratory_pressure) &&
        ensure_live_series(status_.spo2) &&
        ensure_live_series(status_.pulse);
    if (!ok) release_batches();
    return ok;
}

void LiveChartService::release_batches() {
    release_live_series(status_.pressure);
    release_live_series(status_.flow);
    release_live_series(status_.leak);
    release_live_series(status_.inspiratory_pressure);
    release_live_series(status_.expiratory_pressure);
    release_live_series(status_.spo2);
    release_live_series(status_.pulse);
}

void LiveChartService::attach_stream(uint32_t now_ms) {
    if (status_.handle != STREAM_CONSUMER_INVALID &&
        stream_->consumer_active(status_.handle)) {
        if (!status_.attached) {
            status_.attached = true;
            status_.state_dirty = true;
        }
        return;
    }
    status_.handle = STREAM_CONSUMER_INVALID;
    if (status_.attached) {
        status_.attached = false;
        status_.state_dirty = true;
    }
    if (static_cast<int32_t>(now_ms - next_attach_ms_) < 0) return;

    next_attach_ms_ = now_ms + LIVE_CHART_ATTACH_RETRY_MS;
    const std::string params =
        build_stream_params(LIVE_CHART_STREAM_IDS, 40, 200);
    StreamAcquireResult result =
        stream_->acquire(params, RpcSource::Live);
    if (result.status == StreamAcquireStatus::Acquired ||
        result.status == StreamAcquireStatus::AlreadyActive) {
        status_.handle = result.handle;
        status_.attached = true;
        status_.state_dirty = true;
        last_queue_drops_ = 0;
        status_.last_error[0] = 0;
        return;
    }

    status_.attach_failures++;
    status_.state_dirty = true;
    set_error(stream_acquire_status_name(result.status));
}

void LiveChartService::release_stream() {
    if (!stream_) return;
    if (status_.handle != STREAM_CONSUMER_INVALID &&
        stream_->consumer_active(status_.handle)) {
        stream_->release(status_.handle);
    }
    const bool was_attached =
        status_.handle != STREAM_CONSUMER_INVALID || status_.attached;
    status_.handle = STREAM_CONSUMER_INVALID;
    status_.attached = false;
    last_queue_drops_ = 0;
    if (was_attached) status_.state_dirty = true;
}

void LiveChartService::drain_stream(uint32_t now_ms) {
    if (status_.handle == STREAM_CONSUMER_INVALID ||
        !stream_->consumer_active(status_.handle)) {
        if (status_.attached) status_.state_dirty = true;
        status_.attached = false;
        return;
    }

    const uint32_t queue_drops =
        stream_->consumer_queue_drops(status_.handle);
    if (queue_drops < last_queue_drops_) {
        last_queue_drops_ = queue_drops;
    } else if (queue_drops != last_queue_drops_) {
        const uint32_t delta = queue_drops - last_queue_drops_;
        last_queue_drops_ = queue_drops;
        status_.drops += delta;
    }

    for (size_t i = 0; i < AC_WEB_LIVE_FRAME_BUDGET; ++i) {
        StreamFrameRef frame;
        if (!stream_->next_frame(status_.handle, frame)) break;
        if (!frame) continue;

        append_frame_signal(*frame, StreamSignalId::MaskPressure,
                            status_.pressure, status_.drops);
        append_frame_signal(*frame, StreamSignalId::PatientFlow,
                            status_.flow, status_.drops, 60.0f);
        append_frame_signal(*frame, StreamSignalId::Leak,
                            status_.leak, status_.drops, 60.0f);
        if (!append_frame_signal(*frame,
                                 StreamSignalId::InspiratoryPressure,
                                 status_.inspiratory_pressure,
                                 status_.drops)) {
            append_frame_signal(*frame,
                                StreamSignalId::InspiratoryPressureTwoSecond,
                                status_.inspiratory_pressure,
                                status_.drops);
        }
        if (!append_frame_signal(*frame,
                                 StreamSignalId::ExpiratoryPressure,
                                 status_.expiratory_pressure,
                                 status_.drops)) {
            append_frame_signal(*frame,
                                StreamSignalId::ExpiratoryPressureTwoSecond,
                                status_.expiratory_pressure,
                                status_.drops);
        }
        if (!local_oximetry_direct_) {
            append_frame_signal(*frame, StreamSignalId::SpO2,
                                status_.spo2, status_.drops);
            append_frame_signal(*frame, StreamSignalId::HeartRate,
                                status_.pulse, status_.drops);
        }

        status_.frames++;
        status_.last_frame_ms = now_ms;
    }
}

void LiveChartService::set_error(const char *error) {
    copy_cstr(status_.last_error, sizeof(status_.last_error), error);
}

}  // namespace aircannect
