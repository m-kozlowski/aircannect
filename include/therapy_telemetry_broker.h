#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "as11_device_state.h"
#include "session_manager.h"
#include "stream_broker.h"
#include "therapy_telemetry.h"

namespace aircannect {

using TherapyTelemetryMetricMask = uint16_t;

enum TherapyTelemetryMetric : TherapyTelemetryMetricMask {
    THERAPY_METRIC_NONE = 0,
    THERAPY_METRIC_PRESSURE = 1u << 0,
    THERAPY_METRIC_LEAK = 1u << 1,
    THERAPY_METRIC_TIDAL_VOLUME = 1u << 2,
    THERAPY_METRIC_RESPIRATORY_RATE = 1u << 3,
    THERAPY_METRIC_MINUTE_VENTILATION = 1u << 4,
    THERAPY_METRIC_FLOW_LIMITATION = 1u << 5,
    THERAPY_METRIC_INSPIRATORY_DURATION = 1u << 6,
    THERAPY_METRIC_IE_RATIO = 1u << 7,
    THERAPY_METRIC_SNORE = 1u << 8,
};

struct TherapyTelemetryDemand {
    TherapyTelemetryMetricMask metrics = THERAPY_METRIC_NONE;
    uint32_t sample_ms = 2000;
    uint32_t report_ms = 2000;
};

class TherapyTelemetrySubscriber {
public:
    virtual ~TherapyTelemetrySubscriber() = default;

    // Called from the main loop. Implementations must not block.
    virtual void accept_therapy_telemetry(
        const TherapyTelemetrySnapshot &snapshot,
        uint32_t now_ms) = 0;
};

using TherapyTelemetrySubscription = int8_t;
static constexpr TherapyTelemetrySubscription
    THERAPY_TELEMETRY_SUBSCRIPTION_INVALID = -1;

struct TherapyTelemetryRuntimeStatus {
    bool desired = false;
    bool attached = false;
    StreamConsumerHandle stream_handle = STREAM_CONSUMER_INVALID;
    TherapyTelemetryMetricMask requested_metrics = THERAPY_METRIC_NONE;
    uint32_t queue_drops = 0;
    uint32_t attach_failures = 0;
    uint32_t last_frame_ms = 0;
    size_t subscribers = 0;
    char last_error[48] = {};
};

class TherapyTelemetryBroker {
public:
    void begin(StreamBroker &stream,
               const As11DeviceState &device_state,
               SessionManager &session);
    void poll(uint32_t now_ms);

    TherapyTelemetrySubscription subscribe(
        const TherapyTelemetryDemand &demand,
        TherapyTelemetrySubscriber &subscriber);
    bool update(TherapyTelemetrySubscription subscription,
                const TherapyTelemetryDemand &demand);
    void release(TherapyTelemetrySubscription subscription);

    TherapyTelemetrySnapshot snapshot(uint32_t now_ms) const;
    const TherapyTelemetryRuntimeStatus &status() const { return status_; }

private:
    struct Subscriber {
        TherapyTelemetrySubscriber *target = nullptr;
        TherapyTelemetryDemand demand;
        bool active = false;
    };

    static constexpr size_t MAX_SUBSCRIBERS = 4;

    bool subscription_active(TherapyTelemetrySubscription subscription) const;
    int find_free_subscriber() const;
    bool should_run() const;
    bool desired_demand(TherapyTelemetryDemand &demand) const;
    static bool valid_demand(const TherapyTelemetryDemand &demand);
    static std::string stream_ids(
        TherapyTelemetryMetricMask metrics);
    static TherapyTelemetryMetricMask valid_metrics(
        const TherapyTelemetrySnapshot &snapshot);

    void reconcile_stream(uint32_t now_ms);
    void release_stream();
    void drain_stream(uint32_t now_ms);
    void publish_snapshot(uint32_t now_ms);
    void clear_telemetry();
    void set_error(const char *error);

    StreamBroker *stream_ = nullptr;
    const As11DeviceState *device_state_ = nullptr;
    SessionManager *session_ = nullptr;

    Subscriber subscribers_[MAX_SUBSCRIBERS];
    TherapyTelemetry telemetry_;
    TherapyTelemetryRuntimeStatus status_;
    std::string applied_params_;

    uint32_t session_id_ = 0;
    uint32_t next_reconcile_ms_ = 0;
    uint32_t last_queue_drops_ = 0;
    TherapyTelemetryMetricMask published_valid_metrics_ =
        THERAPY_METRIC_NONE;
    bool reconcile_needed_ = true;
    bool publish_pending_ = true;
    bool initialized_ = false;
};

}  // namespace aircannect
