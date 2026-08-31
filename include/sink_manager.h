#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_device_state.h"
#include "session_manager.h"
#include "stream_broker.h"
#include "stream_frame.h"
#include "therapy_telemetry.h"

namespace aircannect {

struct OximetryHubSnapshot;

struct LiveChartSeriesBatch {
    float *values = nullptr;
    uint8_t *valid = nullptr;
    size_t capacity = 0;
    size_t count = 0;
};

struct LiveChartRuntimeStatus {
    bool enabled = false;
    bool desired = false;
    bool attached = false;
    bool state_dirty = true;
    StreamConsumerHandle handle = STREAM_CONSUMER_INVALID;
    uint32_t frames = 0;
    uint32_t drops = 0;
    uint32_t attach_failures = 0;
    uint32_t last_frame_ms = 0;
    char last_error[64] = {};

    LiveChartSeriesBatch pressure;
    LiveChartSeriesBatch flow;
    LiveChartSeriesBatch leak;
    LiveChartSeriesBatch inspiratory_pressure;
    LiveChartSeriesBatch expiratory_pressure;
    LiveChartSeriesBatch spo2;
    LiveChartSeriesBatch pulse;
};

enum TherapyTelemetrySignal : uint8_t {
    THERAPY_TELEMETRY_NONE = 0,
    THERAPY_TELEMETRY_PRESSURE = 1u << 0,
    THERAPY_TELEMETRY_LEAK = 1u << 1,
};

class SinkManager {
public:
    void begin(StreamBroker &stream,
               const As11DeviceState &device_state,
               SessionManager &session);
    void poll(uint32_t now_ms);

    void set_therapy_telemetry_signals(uint8_t signals);
    TherapyTelemetrySnapshot therapy_telemetry_snapshot(
        uint32_t now_ms) const;
    bool take_therapy_telemetry_update(
        uint32_t now_ms,
        TherapyTelemetrySnapshot &snapshot);

    void set_live_chart_enabled(bool enabled);
    void update_local_oximetry(const OximetryHubSnapshot &source,
                               bool mirrored_to_as11);
    const LiveChartRuntimeStatus &live_chart_status() const {
        return live_chart_;
    }
    void clear_live_chart_batch();
    void mark_live_chart_sent();

private:
    bool therapy_telemetry_should_run() const;
    void poll_therapy_telemetry(uint32_t now_ms);
    void attach_therapy_telemetry_stream(uint32_t now_ms);
    void release_therapy_telemetry_stream();
    void drain_therapy_telemetry_stream(uint32_t now_ms);

    bool live_chart_should_run() const;
    void poll_live_chart(uint32_t now_ms);
    bool ensure_live_chart_batches();
    void release_live_chart_batches();
    void attach_live_chart_stream(uint32_t now_ms);
    void release_live_chart_stream();
    void drain_live_chart_stream(uint32_t now_ms);
    void set_live_error(const char *error);

    StreamBroker *stream_ = nullptr;
    const As11DeviceState *device_state_ = nullptr;
    SessionManager *session_ = nullptr;

    TherapyTelemetry therapy_telemetry_;
    StreamConsumerHandle therapy_telemetry_handle_ =
        STREAM_CONSUMER_INVALID;
    uint32_t therapy_telemetry_session_id_ = 0;
    uint32_t next_therapy_telemetry_attach_ms_ = 0;
    uint8_t therapy_telemetry_signals_ = THERAPY_TELEMETRY_NONE;
    bool therapy_telemetry_dirty_ = false;

    LiveChartRuntimeStatus live_chart_;
    uint32_t last_live_queue_drops_ = 0;
    uint32_t last_local_oximetry_ms_ = 0;
    uint32_t next_live_attach_ms_ = 0;
    bool local_oximetry_direct_ = false;
    bool initialized_ = false;
};

}  // namespace aircannect
