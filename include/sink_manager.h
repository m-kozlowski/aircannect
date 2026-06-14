#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "rpc_arbiter.h"
#include "session_manager.h"
#include "stream_frame.h"

namespace aircannect {

struct SinkRuntimeStatus {
    bool debug_enabled = false;
    bool debug_stream_attached = false;
    StreamConsumerHandle debug_stream_handle = STREAM_CONSUMER_INVALID;
    uint32_t attach_attempts = 0;
    uint32_t attach_failures = 0;
    uint32_t frames = 0;
    uint32_t frame_drops = 0;
    uint32_t sessions_started = 0;
    uint32_t sessions_ended = 0;
    uint32_t last_frame_ms = 0;
    char last_error[96] = {};
};

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

struct DebugSinkRuntimeStatus {
    bool enabled = false;
    uint32_t sessions_started = 0;
    uint32_t sessions_ended = 0;
    uint32_t frames = 0;
    uint32_t last_session_id = 0;
    uint32_t last_stream_id = 0;
    uint32_t last_frame_ms = 0;
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual const char *name() const = 0;
    virtual bool enabled() const = 0;
    virtual void set_enabled(bool enabled) = 0;
    virtual void on_session_start(const SessionStatus &session) = 0;
    virtual void on_stream_frame(const SessionStatus &session,
                                 const StreamFrameData &frame) = 0;
    virtual void on_session_end(const SessionStatus &session) = 0;
    virtual void poll() = 0;
};

class SinkManager {
public:
    void begin(RpcArbiter &arbiter, SessionManager &session);
    void poll();

    void set_debug_enabled(bool enabled);
    bool debug_enabled() const;

    void set_live_chart_enabled(bool enabled);
    bool live_chart_enabled() const;
    const LiveChartRuntimeStatus &live_chart_status() const {
        return live_chart_;
    }
    void clear_live_chart_batch();
    void mark_live_chart_sent();

    const SinkRuntimeStatus &status() const { return status_; }
    DebugSinkRuntimeStatus debug_status() const;

private:
    class DebugSink : public Sink {
    public:
        const char *name() const override { return "debug"; }
        bool enabled() const override { return enabled_; }
        void set_enabled(bool enabled) override { enabled_ = enabled; }
        void on_session_start(const SessionStatus &session) override;
        void on_stream_frame(const SessionStatus &session,
                             const StreamFrameData &frame) override;
        void on_session_end(const SessionStatus &session) override;
        void poll() override {}
        DebugSinkRuntimeStatus status() const;

        uint32_t sessions_started() const { return sessions_started_; }
        uint32_t sessions_ended() const { return sessions_ended_; }
        uint32_t frames() const { return frames_; }

    private:
        bool enabled_ = false;
        uint32_t sessions_started_ = 0;
        uint32_t sessions_ended_ = 0;
        uint32_t frames_ = 0;
        uint32_t last_session_id_ = 0;
        uint32_t last_stream_id_ = 0;
        uint32_t last_frame_ms_ = 0;
    };

    void dispatch_session_edges();
    void attach_debug_stream(uint32_t now_ms);
    void release_debug_stream();
    void drain_debug_stream(uint32_t now_ms);
    bool live_chart_should_run() const;
    void poll_live_chart(uint32_t now_ms);
    bool ensure_live_chart_batches();
    void release_live_chart_batches();
    void attach_live_chart_stream(uint32_t now_ms);
    void release_live_chart_stream();
    void drain_live_chart_stream(uint32_t now_ms);
    void set_error(const char *error);
    void set_live_error(const char *error);

    RpcArbiter *arbiter_ = nullptr;
    SessionManager *session_ = nullptr;
    DebugSink debug_sink_;
    SinkRuntimeStatus status_;
    LiveChartRuntimeStatus live_chart_;
    uint32_t seen_session_starts_ = 0;
    uint32_t seen_session_ends_ = 0;
    uint32_t last_debug_queue_drops_ = 0;
    uint32_t last_live_queue_drops_ = 0;
    uint32_t next_attach_ms_ = 0;
    uint32_t next_live_attach_ms_ = 0;
    bool initialized_ = false;
};

}  // namespace aircannect
