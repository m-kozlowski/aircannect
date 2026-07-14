#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "rpc_arbiter.h"
#include "session_manager.h"
#include "stream_frame.h"

namespace aircannect {

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

class SinkManager {
public:
    void begin(RpcArbiter &arbiter, SessionManager &session);
    void poll();

    void set_live_chart_enabled(bool enabled);
    const LiveChartRuntimeStatus &live_chart_status() const {
        return live_chart_;
    }
    void clear_live_chart_batch();
    void mark_live_chart_sent();

private:
    bool live_chart_should_run() const;
    void poll_live_chart(uint32_t now_ms);
    bool ensure_live_chart_batches();
    void release_live_chart_batches();
    void attach_live_chart_stream(uint32_t now_ms);
    void release_live_chart_stream();
    void drain_live_chart_stream(uint32_t now_ms);
    void set_live_error(const char *error);

    RpcArbiter *arbiter_ = nullptr;
    SessionManager *session_ = nullptr;
    LiveChartRuntimeStatus live_chart_;
    uint32_t last_live_queue_drops_ = 0;
    uint32_t next_live_attach_ms_ = 0;
    bool initialized_ = false;
};

}  // namespace aircannect
