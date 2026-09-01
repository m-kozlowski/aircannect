#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_device_state.h"
#include "session_manager.h"
#include "stream_broker.h"
#include "stream_frame.h"

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

class LiveChartService {
public:
    void begin(StreamBroker &stream,
               const As11DeviceState &device_state,
               SessionManager &session);
    void poll(uint32_t now_ms);

    void set_enabled(bool enabled);
    void update_local_oximetry(const OximetryHubSnapshot &source,
                               bool mirrored_to_as11);
    const LiveChartRuntimeStatus &status() const { return status_; }
    void clear_batch();
    void mark_sent();
    void reset_counters();

private:
    bool should_run() const;
    bool ensure_batches();
    void release_batches();
    void attach_stream(uint32_t now_ms);
    void release_stream();
    void drain_stream(uint32_t now_ms);
    void set_error(const char *error);

    StreamBroker *stream_ = nullptr;
    const As11DeviceState *device_state_ = nullptr;
    SessionManager *session_ = nullptr;

    LiveChartRuntimeStatus status_;
    uint32_t last_queue_drops_ = 0;
    uint32_t last_local_oximetry_ms_ = 0;
    uint32_t next_attach_ms_ = 0;
    bool local_oximetry_direct_ = false;
    bool initialized_ = false;
};

}  // namespace aircannect
