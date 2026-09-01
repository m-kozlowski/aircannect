#include "therapy_telemetry_broker.h"

#include "as11_rpc.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr TherapyTelemetryMetricMask THERAPY_METRIC_ALL =
    THERAPY_METRIC_PRESSURE |
    THERAPY_METRIC_LEAK |
    THERAPY_METRIC_TIDAL_VOLUME |
    THERAPY_METRIC_RESPIRATORY_RATE |
    THERAPY_METRIC_MINUTE_VENTILATION |
    THERAPY_METRIC_FLOW_LIMITATION |
    THERAPY_METRIC_INSPIRATORY_DURATION |
    THERAPY_METRIC_IE_RATIO |
    THERAPY_METRIC_SNORE;

constexpr uint32_t TELEMETRY_RETRY_MS = 2000;
constexpr uint32_t TELEMETRY_BUSY_RETRY_MS = 100;
constexpr size_t TELEMETRY_FRAME_BUDGET = 4;

void append_stream_id(std::string &out, const char *id) {
    if (!out.empty()) out.push_back(',');
    out += id;
}

}  // namespace

void TherapyTelemetryBroker::begin(StreamBroker &stream,
                                   const As11DeviceState &device_state,
                                   SessionManager &session) {
    if (initialized_) return;

    stream_ = &stream;
    device_state_ = &device_state;
    session_ = &session;
    initialized_ = true;
}

void TherapyTelemetryBroker::poll(uint32_t now_ms) {
    if (!initialized_ || !stream_ || !device_state_ || !session_) return;

    TherapyTelemetryDemand demand;
    const bool have_demand = desired_demand(demand);
    status_.desired = have_demand && should_run();
    status_.requested_metrics = have_demand
        ? demand.metrics
        : TherapyTelemetryMetricMask{};

    if (!status_.desired) {
        release_stream();
        if (session_id_ != 0) {
            session_id_ = 0;
            clear_telemetry();
        }
        publish_snapshot(now_ms);
        return;
    }

    const uint32_t current_session = session_->status().session_id;
    if (current_session != session_id_) {
        session_id_ = current_session;
        clear_telemetry();
    }

    reconcile_stream(now_ms);
    drain_stream(now_ms);
    publish_snapshot(now_ms);
}

TherapyTelemetrySubscription TherapyTelemetryBroker::subscribe(
    const TherapyTelemetryDemand &demand,
    TherapyTelemetrySubscriber &subscriber) {
    if (!valid_demand(demand)) {
        return THERAPY_TELEMETRY_SUBSCRIPTION_INVALID;
    }

    for (size_t i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (!subscribers_[i].active ||
            subscribers_[i].target != &subscriber) {
            continue;
        }
        subscribers_[i].demand = demand;
        reconcile_needed_ = true;
        publish_pending_ = true;
        return static_cast<TherapyTelemetrySubscription>(i);
    }

    const int slot = find_free_subscriber();
    if (slot < 0) return THERAPY_TELEMETRY_SUBSCRIPTION_INVALID;

    subscribers_[slot].active = true;
    subscribers_[slot].target = &subscriber;
    subscribers_[slot].demand = demand;
    status_.subscribers++;
    reconcile_needed_ = true;
    publish_pending_ = true;
    return static_cast<TherapyTelemetrySubscription>(slot);
}

bool TherapyTelemetryBroker::update(
    TherapyTelemetrySubscription subscription,
    const TherapyTelemetryDemand &demand) {
    if (!subscription_active(subscription) || !valid_demand(demand)) {
        return false;
    }
    if (subscribers_[subscription].demand.metrics == demand.metrics &&
        subscribers_[subscription].demand.sample_ms == demand.sample_ms &&
        subscribers_[subscription].demand.report_ms == demand.report_ms) {
        return true;
    }

    subscribers_[subscription].demand = demand;
    reconcile_needed_ = true;
    return true;
}

void TherapyTelemetryBroker::release(
    TherapyTelemetrySubscription subscription) {
    if (!subscription_active(subscription)) return;

    subscribers_[subscription] = {};
    if (status_.subscribers > 0) status_.subscribers--;
    reconcile_needed_ = true;
}

TherapyTelemetrySnapshot TherapyTelemetryBroker::snapshot(
    uint32_t now_ms) const {
    return telemetry_.snapshot(now_ms);
}

bool TherapyTelemetryBroker::subscription_active(
    TherapyTelemetrySubscription subscription) const {
    return subscription >= 0 &&
           subscription <
               static_cast<TherapyTelemetrySubscription>(MAX_SUBSCRIBERS) &&
           subscribers_[subscription].active;
}

int TherapyTelemetryBroker::find_free_subscriber() const {
    for (size_t i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (!subscribers_[i].active) return static_cast<int>(i);
    }
    return -1;
}

bool TherapyTelemetryBroker::should_run() const {
    if (device_state_->therapy_state() == As11TherapyState::Running) {
        return true;
    }
    return session_->status().state == SessionState::Active;
}

bool TherapyTelemetryBroker::desired_demand(
    TherapyTelemetryDemand &demand) const {
    demand = {};
    bool found = false;

    for (const Subscriber &subscriber : subscribers_) {
        if (!subscriber.active) continue;

        demand.metrics |= subscriber.demand.metrics;
        if (!found || subscriber.demand.sample_ms < demand.sample_ms) {
            demand.sample_ms = subscriber.demand.sample_ms;
        }
        if (!found || subscriber.demand.report_ms < demand.report_ms) {
            demand.report_ms = subscriber.demand.report_ms;
        }
        found = true;
    }

    return found && demand.metrics != THERAPY_METRIC_NONE;
}

bool TherapyTelemetryBroker::valid_demand(
    const TherapyTelemetryDemand &demand) {
    return demand.metrics != THERAPY_METRIC_NONE &&
           (demand.metrics & ~THERAPY_METRIC_ALL) == 0 &&
           demand.sample_ms != 0 && demand.report_ms != 0;
}

std::string TherapyTelemetryBroker::stream_ids(
    TherapyTelemetryMetricMask metrics) {
    std::string out;
    if (metrics & THERAPY_METRIC_PRESSURE) {
        append_stream_id(out, "_MKI");
        append_stream_id(out, "_MKE");
    }
    if (metrics & THERAPY_METRIC_LEAK) {
        append_stream_id(out, "_LKF");
    }
    if (metrics & THERAPY_METRIC_TIDAL_VOLUME) {
        append_stream_id(out, "_TD2");
    }
    if (metrics & THERAPY_METRIC_RESPIRATORY_RATE) {
        append_stream_id(out, "_RR2");
    }
    if (metrics & THERAPY_METRIC_MINUTE_VENTILATION) {
        append_stream_id(out, "_MV2");
    }
    if (metrics & THERAPY_METRIC_FLOW_LIMITATION) {
        append_stream_id(out, "_FFL");
    }
    if (metrics & THERAPY_METRIC_INSPIRATORY_DURATION) {
        append_stream_id(out, "_INT");
    }
    if (metrics & THERAPY_METRIC_IE_RATIO) {
        append_stream_id(out, "_IE2");
    }
    if (metrics & THERAPY_METRIC_SNORE) {
        append_stream_id(out, "_SNI");
    }
    return out;
}

TherapyTelemetryMetricMask TherapyTelemetryBroker::valid_metrics(
    const TherapyTelemetrySnapshot &snapshot) {
    TherapyTelemetryMetricMask metrics = THERAPY_METRIC_NONE;
    if (snapshot.inspiratory_pressure_valid ||
        snapshot.expiratory_pressure_valid) {
        metrics |= THERAPY_METRIC_PRESSURE;
    }
    if (snapshot.leak_valid) metrics |= THERAPY_METRIC_LEAK;
    if (snapshot.tidal_volume_valid) metrics |= THERAPY_METRIC_TIDAL_VOLUME;
    if (snapshot.respiratory_rate_valid) {
        metrics |= THERAPY_METRIC_RESPIRATORY_RATE;
    }
    if (snapshot.minute_ventilation_valid) {
        metrics |= THERAPY_METRIC_MINUTE_VENTILATION;
    }
    if (snapshot.flow_limitation_valid) {
        metrics |= THERAPY_METRIC_FLOW_LIMITATION;
    }
    if (snapshot.inspiratory_duration_valid) {
        metrics |= THERAPY_METRIC_INSPIRATORY_DURATION;
    }
    if (snapshot.ie_ratio_valid) metrics |= THERAPY_METRIC_IE_RATIO;
    if (snapshot.snore_valid) metrics |= THERAPY_METRIC_SNORE;
    return metrics;
}

void TherapyTelemetryBroker::reconcile_stream(uint32_t now_ms) {
    if (!reconcile_needed_) return;
    if (static_cast<int32_t>(now_ms - next_reconcile_ms_) < 0) return;

    TherapyTelemetryDemand demand;
    if (!desired_demand(demand)) {
        release_stream();
        reconcile_needed_ = false;
        return;
    }

    const std::string params = build_stream_params(
        stream_ids(demand.metrics), demand.sample_ms, demand.report_ms);

    if (status_.stream_handle != STREAM_CONSUMER_INVALID &&
        !stream_->consumer_active(status_.stream_handle)) {
        status_.stream_handle = STREAM_CONSUMER_INVALID;
        status_.attached = false;
        applied_params_.clear();
    }

    StreamAcquireResult result;
    if (status_.stream_handle == STREAM_CONSUMER_INVALID) {
        result = stream_->acquire(params, RpcSource::Telemetry);
    } else if (applied_params_ == params) {
        status_.attached = true;
        reconcile_needed_ = false;
        return;
    } else {
        result = stream_->update(status_.stream_handle, params);
    }

    if (result.status == StreamAcquireStatus::Acquired ||
        result.status == StreamAcquireStatus::AlreadyActive) {
        status_.stream_handle = result.handle;
        status_.attached = true;
        status_.last_error[0] = 0;
        applied_params_ = params;
        last_queue_drops_ = 0;
        reconcile_needed_ = false;
        return;
    }

    if (result.status == StreamAcquireStatus::Busy) {
        next_reconcile_ms_ = now_ms + TELEMETRY_BUSY_RETRY_MS;
        return;
    }

    status_.attach_failures++;
    set_error(stream_acquire_status_name(result.status));
    next_reconcile_ms_ = now_ms + TELEMETRY_RETRY_MS;
}

void TherapyTelemetryBroker::release_stream() {
    if (stream_ && status_.stream_handle != STREAM_CONSUMER_INVALID &&
        stream_->consumer_active(status_.stream_handle)) {
        stream_->release(status_.stream_handle);
    }

    status_.stream_handle = STREAM_CONSUMER_INVALID;
    status_.attached = false;
    applied_params_.clear();
    last_queue_drops_ = 0;
    reconcile_needed_ = true;
}

void TherapyTelemetryBroker::drain_stream(uint32_t now_ms) {
    if (status_.stream_handle == STREAM_CONSUMER_INVALID ||
        !stream_->consumer_active(status_.stream_handle)) {
        status_.attached = false;
        reconcile_needed_ = true;
        return;
    }

    const uint32_t queue_drops =
        stream_->consumer_queue_drops(status_.stream_handle);
    if (queue_drops < last_queue_drops_) {
        last_queue_drops_ = queue_drops;
    } else if (queue_drops != last_queue_drops_) {
        status_.queue_drops += queue_drops - last_queue_drops_;
        last_queue_drops_ = queue_drops;
    }

    for (size_t i = 0; i < TELEMETRY_FRAME_BUDGET; ++i) {
        StreamFrameRef frame;
        if (!stream_->next_frame(status_.stream_handle, frame)) break;
        if (!frame) continue;

        if (telemetry_.accept_frame(*frame, now_ms)) {
            publish_pending_ = true;
        }
        status_.last_frame_ms = now_ms;
    }
}

void TherapyTelemetryBroker::publish_snapshot(uint32_t now_ms) {
    const TherapyTelemetrySnapshot current = snapshot(now_ms);
    const TherapyTelemetryMetricMask current_valid = valid_metrics(current);
    if (current_valid != published_valid_metrics_) {
        publish_pending_ = true;
    }
    if (!publish_pending_) return;

    published_valid_metrics_ = current_valid;
    publish_pending_ = false;
    for (Subscriber &subscriber : subscribers_) {
        if (!subscriber.active || !subscriber.target) continue;
        subscriber.target->accept_therapy_telemetry(current, now_ms);
    }
}

void TherapyTelemetryBroker::clear_telemetry() {
    telemetry_.clear();
    published_valid_metrics_ = THERAPY_METRIC_NONE;
    publish_pending_ = true;
}

void TherapyTelemetryBroker::set_error(const char *error) {
    copy_cstr(status_.last_error, sizeof(status_.last_error), error);
}

}  // namespace aircannect
