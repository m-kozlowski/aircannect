#include "rpc_arbiter.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "as11_rpc.h"
#include "debug_log.h"

namespace aircannect {
namespace {

bool emit_matched_response(RpcSource source) {
    return source == RpcSource::Console ||
           source == RpcSource::Tcp ||
           source == RpcSource::HttpApi;
}

bool scheduler_source(RpcSource source) {
    return source == RpcSource::Scheduler || source == RpcSource::Internal;
}

bool current_epoch_ms(int64_t &epoch_ms) {
    struct timeval tv = {};
    if (gettimeofday(&tv, nullptr) != 0) return false;
    if (tv.tv_sec < 1609459200) return false;
    epoch_ms = static_cast<int64_t>(tv.tv_sec) * 1000 +
               static_cast<int64_t>(tv.tv_usec / 1000);
    return true;
}

std::string format_utc_ms(int64_t epoch_ms) {
    if (epoch_ms < 1609459200000LL) return "";
    const time_t epoch = static_cast<time_t>(epoch_ms / 1000);
    struct tm utc = {};
    gmtime_r(&epoch, &utc);
    char base[25];
    if (strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
        return "";
    }
    char out[29];
    snprintf(out, sizeof(out), "%s.%03dZ", base,
             static_cast<int>(epoch_ms % 1000));
    return std::string(out);
}

std::string current_utc_iso_ms() {
    int64_t epoch_ms = 0;
    if (!current_epoch_ms(epoch_ms)) return "";
    return format_utc_ms(epoch_ms);
}

std::string current_utc_iso_nearest_second() {
    int64_t epoch_ms = 0;
    if (!current_epoch_ms(epoch_ms)) return "";
    const int64_t rounded_ms = ((epoch_ms + 500) / 1000) * 1000;
    return format_utc_ms(rounded_ms);
}

const char *const SETTINGS_HISTORY_CHANGE_DATA_ID =
    "SettingsHistoryChangeCount";

const char *const AS11_EVENT_SUBSCRIBE_PARAMS =
    "{\"dataIds\":[\"SystemActivityEvents-FrequentActivityEvents\","
    "\"SystemActivityEvents-SporadicActivityEvents\","
    "\"SettingsHistoryChangeCount\"]}";

bool settings_history_change_notification(const std::string &payload) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return false;

    const char *method = doc["method"].as<const char *>();
    if (!method || strcmp(method, "EventNotification") != 0) return false;

    JsonObjectConst params = doc["params"].as<JsonObjectConst>();
    if (params.isNull()) return false;
    const char *data_id = params["dataId"].as<const char *>();
    if (!data_id || strcmp(data_id, SETTINGS_HISTORY_CHANGE_DATA_ID) != 0) {
        return false;
    }

    JsonArrayConst events = params["events"].as<JsonArrayConst>();
    for (JsonObjectConst event : events) {
        const char *name = event["event"].as<const char *>();
        if (name && strcmp(name, "ValueChange") == 0) return true;
    }
    return false;
}

bool event_suggests_identity_refresh(const std::string &event) {
    return event == "PowerUp" ||
           event == "SettingsReset" ||
           event == "ResetToDefaultsComplete" ||
           event == "RpcEraseData";
}

bool event_suggests_status_refresh(const std::string &event) {
    return event == "TherapyStarted" ||
           event == "StandbyStarted" ||
           event == "MaskfitStarted" ||
           event == "TestDriveStarted" ||
           event == "CalibrationStarted";
}

bool event_suggests_motor_refresh(const std::string &event) {
    return event == "StandbyStarted" || event == "CooldownStopped";
}

}  // namespace

RpcArbiter::RpcArbiter(CanDriver &can) : can_(can) {
    stats_started_ms_ = millis();
}

void RpcArbiter::poll() {
    can_.poll();
    const uint32_t now = millis();

    DatagramFeedResult rpc_timeout = rpc_rx_.poll(now);
    if (rpc_timeout.status == DatagramStatus::Error) {
        stats_.rpc_framing_errors++;
        push_event(RpcEventKind::FramingError,
                   std::string("[RPC] ") + rpc_timeout.error);
    }

    DatagramFeedResult log_timeout = log_rx_.poll(now);
    if (log_timeout.status == DatagramStatus::Error) {
        stats_.log_framing_errors++;
        push_event(RpcEventKind::FramingError,
                   std::string("[LOG] ") + log_timeout.error);
    }

    for (size_t drained = 0; drained < AC_CAN_RX_DRAIN_BUDGET; ++drained) {
        RawCanFrame frame;
        if (!can_.receive(frame, 0)) break;
        handle_frame(frame);
    }

    check_pending_timeout();
    poll_stream_subscription();
    poll_event_subscription();
    poll_as11_healthcheck();
    dispatch_next_request();

    // Dispatch may enqueue CAN frames after RX-driven work above. Poll again so
    // the CAN driver can start TX in the same main-loop turn.
    can_.poll();
}

bool RpcArbiter::submit_raw_payload(const std::string &payload, RpcSource source) {
    if (Log::get_cat_level(CAT_RPC) >= LOG_DEBUG) {
        char prefix[80];
        snprintf(prefix, sizeof(prefix), "[RPC raw request source=%s] ",
                 source_name(source));
        Log::log_payload(CAT_RPC, LOG_DEBUG, prefix, payload);
    }
    if (!enqueue_payload_frames(payload, source)) return false;

    uint32_t id = 0;
    if ((source == RpcSource::Console || source == RpcSource::Tcp) &&
        json_extract_id(payload, id)) {
        remember_raw_passthrough(id, source, millis());
    }
    return true;
}

bool RpcArbiter::enqueue_payload_frames(const std::string &payload,
                                        RpcSource source) {
    (void)source;
    const auto frames = encode_datagram(payload);
    if (frames.size() > can_.tx_queue_free()) {
        push_event(RpcEventKind::Info, "CAN TX queue full; payload rejected");
        return false;
    }

    return enqueue_encoded_frames(frames);
}

bool RpcArbiter::enqueue_encoded_frames(
    const std::vector<DatagramFrame> &frames) {
    for (const auto &frame : frames) {
        RawCanFrame raw;
        raw.id = AC_CAN_TX_ID;
        raw.len = frame.len;
        for (uint8_t i = 0; i < frame.len; ++i) raw.data[i] = frame.data[i];
        if (!can_.enqueue_tx(raw)) {
            push_event(RpcEventKind::Info, "CAN TX enqueue failed");
            return false;
        }
    }

    return true;
}

bool RpcArbiter::send_request(const std::string &method,
                              const std::string &params_json,
                              RpcSource source,
                              uint32_t timeout_ms) {
    QueuedRequest request;
    request.method = method;
    request.params_json = params_json;
    request.source = source;
    request.timeout_ms = timeout_ms ? timeout_ms :
        ((method == "StartStream") ? AC_RPC_STREAM_TIMEOUT_MS
                                   : AC_RPC_DEFAULT_TIMEOUT_MS);

    return enqueue_request(request);
}

bool RpcArbiter::send_set_datetime_now(RpcSource source,
                                       uint32_t timeout_ms) {
    QueuedRequest request;
    request.method = "SetDateTime";
    request.source = source;
    request.timeout_ms = timeout_ms ? timeout_ms : AC_RPC_DEFAULT_TIMEOUT_MS;
    request.set_datetime_now = true;
    return enqueue_request(request);
}

bool RpcArbiter::enqueue_request(QueuedRequest &request) {
    const uint32_t now = millis();
    if (scheduler_source(request.source) && background_backoff_active(now)) {
        return false;
    }

    request.id = ++next_rpc_id_;
    if (!requests_.push(request)) {
        stats_.request_queue_drops++;
        push_event(RpcEventKind::Info, "RPC request queue full");
        return false;
    }

    stats_.queued_requests++;
    return true;
}

bool RpcArbiter::next_event(RpcEvent &event) {
    return events_.pop(event);
}

bool RpcArbiter::next_resmed_ota_event(RpcEvent &event) {
    return resmed_ota_events_.pop(event);
}

StreamAcquireResult RpcArbiter::acquire_stream(const std::string &params_json,
                                               RpcSource source) {
    StreamAcquireResult result = stream_.acquire(params_json, source_id(source));
    if (result.status == StreamAcquireStatus::Incompatible ||
        result.status == StreamAcquireStatus::Full ||
        result.status == StreamAcquireStatus::Rejected) {
        stats_.stream_consumer_rejects++;
    }
    return result;
}

StreamAcquireResult RpcArbiter::update_stream(StreamConsumerHandle handle,
                                              const std::string &params_json) {
    StreamAcquireResult result = stream_.update(handle, params_json);
    if (result.status == StreamAcquireStatus::Incompatible ||
        result.status == StreamAcquireStatus::Busy ||
        result.status == StreamAcquireStatus::Rejected) {
        stats_.stream_consumer_rejects++;
    }
    return result;
}

void RpcArbiter::release_stream(StreamConsumerHandle handle) {
    stream_.release(handle);
}

bool RpcArbiter::stream_consumer_active(StreamConsumerHandle handle) const {
    return stream_.consumer_active(handle);
}

uint32_t RpcArbiter::stream_consumer_queue_drops(
    StreamConsumerHandle handle) const {
    return stream_.consumer_queue_drops(handle);
}

bool RpcArbiter::stream_activity_active() const {
    if (stream_.desired_active() || stream_.actual_active() ||
        stream_.pending()) {
        return true;
    }
    return stream_.last_notification_ms() &&
           millis() - stream_.last_notification_ms() <
               AC_WIFI_ROAM_STREAM_QUIET_MS;
}

void RpcArbiter::set_stream_frame_observer(StreamFrameObserver observer,
                                           void *context) {
    stream_.set_frame_observer(observer, context);
}

bool RpcArbiter::next_stream_frame(StreamConsumerHandle handle,
                                   StreamFrameRef &frame) {
    return stream_.next_frame(handle, frame);
}

bool RpcArbiter::next_stream_payload(StreamConsumerHandle handle,
                                     std::string &payload) {
    return stream_.next_payload(handle, payload);
}

void RpcArbiter::reset_stats() {
    stats_ = {};
    can_.reset_stats();
    stream_.reset_counters();
    consecutive_scheduler_timeouts_ = 0;
    background_backoff_until_ms_ = 0;
    stats_started_ms_ = millis();
}

RpcRuntimeStatus RpcArbiter::runtime_status() const {
    const uint32_t now = millis();
    RpcRuntimeStatus out;
    out.stats_elapsed_ms = std::max<uint32_t>(1, now - stats_started_ms_);
    out.request_queue_depth = requests_.count();
    out.pending_request_id = pending_.active ? pending_.id : 0;
    out.dispatch_retry_id = dispatch_retry_active_ ? dispatch_retry_.id : 0;
    out.background_backoff_ms = background_backoff_active(now)
        ? background_backoff_until_ms_ - now
        : 0;
    out.event_subscription_active = event_subscription_active_;
    out.event_subscription_id = event_subscription_id_;
    out.boot_notifications = boot_notifications_seen_;
    if (!last_boot_notification_.empty()) {
        out.last_boot_notification_age_ms = now - last_boot_notification_ms_;
        out.last_boot_notification = last_boot_notification_;
    }
    return out;
}

bool RpcArbiter::recover_can(const char *reason) {
    rpc_rx_.reset();
    log_rx_.reset();
    cancel_all_requests(reason ? reason : "can_recovery");
    stream_.mark_reattach();
    event_subscription_active_ = false;
    event_subscription_id_ = 0;
    next_event_subscribe_ms_ = millis() + AC_AS11_EVENT_SUBSCRIBE_DELAY_MS;
    if (as11_state_.therapy_command_pending()) {
        as11_state_.clear_pending_therapy_command("can_recovery", millis());
    }
    return can_.recover_or_restart(reason);
}

void RpcArbiter::set_background_polls_suspended(bool suspended) {
    background_polls_suspended_ = suspended;
}

void RpcArbiter::cancel_requests_from_source(RpcSource source,
                                             const char *reason) {
    const char *why = reason ? reason : "cancelled";
    if (pending_.active && pending_.source == source) {
        cancel_pending_request(why);
    }
    if (dispatch_retry_active_ && dispatch_retry_.source == source) {
        cancel_queued_request(dispatch_retry_, why);
        dispatch_retry_ = {};
        dispatch_retry_active_ = false;
        dispatch_retry_deadline_ms_ = 0;
        next_dispatch_retry_ms_ = 0;
    }

    QueuedRequest request;
    FixedQueue<QueuedRequest, AC_RPC_REQUEST_QUEUE_DEPTH> keep;
    while (requests_.pop(request)) {
        if (request.source == source) {
            cancel_queued_request(request, why);
        } else {
            keep.push(request);
        }
    }
    while (keep.pop(request)) {
        requests_.push(request);
    }
}

bool RpcArbiter::background_backoff_active(uint32_t now) const {
    return background_backoff_until_ms_ &&
           static_cast<int32_t>(background_backoff_until_ms_ - now) > 0;
}

void RpcArbiter::note_request_success(RpcSource source, uint32_t now) {
    (void)now;
    (void)source;
    consecutive_scheduler_timeouts_ = 0;
    background_backoff_until_ms_ = 0;
}

void RpcArbiter::note_request_timeout(RpcSource source, uint32_t now) {
    if (!scheduler_source(source)) return;
    if (consecutive_scheduler_timeouts_ < 255) {
        consecutive_scheduler_timeouts_++;
    }
    if (consecutive_scheduler_timeouts_ <
        AC_RPC_BACKGROUND_TIMEOUTS_BEFORE_BACKOFF) {
        return;
    }
    background_backoff_until_ms_ = now + AC_RPC_BACKGROUND_BACKOFF_MS;
    stats_.background_backoffs++;
    if (next_as11_identity_poll_ms_) {
        next_as11_identity_poll_ms_ = background_backoff_until_ms_;
    }
    next_as11_status_poll_ms_ =
        background_backoff_until_ms_ + AC_RPC_DEFAULT_TIMEOUT_MS;
    if (next_as11_motor_poll_ms_) {
        next_as11_motor_poll_ms_ =
            background_backoff_until_ms_ + (AC_RPC_DEFAULT_TIMEOUT_MS * 2);
    }
    next_as11_timezone_poll_ms_ =
        background_backoff_until_ms_ + (AC_RPC_DEFAULT_TIMEOUT_MS * 3);
    next_as11_clock_poll_ms_ =
        background_backoff_until_ms_ + (AC_RPC_DEFAULT_TIMEOUT_MS * 4);
    Log::logf(CAT_RPC, LOG_WARN,
              "[RPC] background polling paused for %lu ms after %u timeouts\n",
              static_cast<unsigned long>(AC_RPC_BACKGROUND_BACKOFF_MS),
              static_cast<unsigned>(consecutive_scheduler_timeouts_));
}

void RpcArbiter::schedule_as11_identity_refresh(uint32_t now,
                                                uint32_t delay_ms) {
    const uint32_t due_ms = now + delay_ms;
    if (!next_as11_identity_poll_ms_ ||
        static_cast<int32_t>(due_ms - next_as11_identity_poll_ms_) < 0) {
        next_as11_identity_poll_ms_ = due_ms;
    }
}

void RpcArbiter::schedule_as11_status_refresh(uint32_t now,
                                              uint32_t delay_ms) {
    const uint32_t due_ms = now + delay_ms;
    if (!next_as11_status_poll_ms_ ||
        static_cast<int32_t>(due_ms - next_as11_status_poll_ms_) < 0) {
        next_as11_status_poll_ms_ = due_ms;
    }
}

void RpcArbiter::schedule_as11_motor_refresh(uint32_t now,
                                             uint32_t delay_ms) {
    const uint32_t due_ms = now + delay_ms;
    if (!next_as11_motor_poll_ms_ ||
        static_cast<int32_t>(due_ms - next_as11_motor_poll_ms_) < 0) {
        next_as11_motor_poll_ms_ = due_ms;
    }
}

void RpcArbiter::schedule_as11_timezone_refresh(uint32_t now,
                                                uint32_t delay_ms) {
    const uint32_t due_ms = now + delay_ms;
    if (!next_as11_timezone_poll_ms_ ||
        static_cast<int32_t>(due_ms - next_as11_timezone_poll_ms_) < 0) {
        next_as11_timezone_poll_ms_ = due_ms;
    }
}

void RpcArbiter::update_as11_motor_refresh_after_state(uint32_t now) {
    if (as11_state_.therapy_state() == As11TherapyState::Running) {
        if (!next_as11_motor_poll_ms_) {
            next_as11_motor_poll_ms_ =
                now + AC_AS11_MOTOR_RUNTIME_POLL_INTERVAL_MS;
        }
    }
}

void RpcArbiter::push_event(RpcEventKind kind,
                            const std::string &payload,
                            RpcSource source,
                            uint32_t id) {
    RpcEvent event;
    event.kind = kind;
    event.source = source;
    event.id = id;
    event.payload = payload;
    if (!events_.push(event)) stats_.event_drops++;
}

void RpcArbiter::push_resmed_ota_event(RpcEventKind kind,
                                       const std::string &payload,
                                       RpcSource source,
                                       uint32_t id) {
    RpcEvent event;
    event.kind = kind;
    event.source = source;
    event.id = id;
    event.payload = payload;
    if (!resmed_ota_events_.push(event)) stats_.event_drops++;
}

void RpcArbiter::cancel_pending_request(const char *reason) {
    if (!pending_.active) return;
    stats_.request_cancellations++;
    if (pending_.source == RpcSource::ResmedOta) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(pending_.id),
                 pending_.method.c_str(),
                 source_name(pending_.source),
                 reason ? reason : "unknown");
        push_resmed_ota_event(RpcEventKind::Info, buf);
    } else if (pending_.source != RpcSource::Scheduler) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(pending_.id),
                 pending_.method.c_str(),
                 source_name(pending_.source),
                 reason ? reason : "unknown");
        push_event(RpcEventKind::Info, buf);
    }
    if (pending_.stream_command != StreamCommandType::None) {
        stream_.mark_command_timeout(millis());
    }
    if (pending_.method == "SubscribeEvent") {
        event_subscription_active_ = false;
        event_subscription_id_ = 0;
        next_event_subscribe_ms_ =
            millis() + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
        stats_.event_subscribe_errors++;
    }
    as11_state_.mark_therapy_command_timeout(pending_.method, millis());
    if (pending_.method == "Set") {
        as11_settings_.note_set_cancelled(reason ? reason : "cancelled",
                                          millis());
    }
    pending_ = {};
}

void RpcArbiter::cancel_queued_request(const QueuedRequest &request,
                                       const char *reason) {
    stats_.request_cancellations++;
    if (request.source == RpcSource::ResmedOta) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(request.id),
                 request.method.c_str(),
                 source_name(request.source),
                 reason ? reason : "unknown");
        push_resmed_ota_event(RpcEventKind::Info, buf);
    } else if (request.source != RpcSource::Scheduler) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(request.id),
                 request.method.c_str(),
                 source_name(request.source),
                 reason ? reason : "unknown");
        push_event(RpcEventKind::Info, buf);
    }
    if (request.stream_command != StreamCommandType::None) {
        stream_.mark_command_timeout(millis());
    }
    if (request.method == "SubscribeEvent") {
        event_subscription_active_ = false;
        event_subscription_id_ = 0;
        next_event_subscribe_ms_ =
            millis() + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
        stats_.event_subscribe_errors++;
    }
}

void RpcArbiter::cancel_all_requests(const char *reason) {
    cancel_pending_request(reason);
    if (dispatch_retry_active_) {
        cancel_queued_request(dispatch_retry_, reason);
        dispatch_retry_ = {};
        dispatch_retry_active_ = false;
        dispatch_retry_deadline_ms_ = 0;
        next_dispatch_retry_ms_ = 0;
    }

    QueuedRequest request;
    while (requests_.pop(request)) {
        cancel_queued_request(request, reason);
    }
    for (auto &raw : raw_passthrough_) raw = {};
}

void RpcArbiter::expire_raw_passthrough(uint32_t now) {
    for (auto &request : raw_passthrough_) {
        if (!request.active) continue;
        if (static_cast<int32_t>(now - request.deadline_ms) >= 0) {
            request = {};
        }
    }
}

void RpcArbiter::remember_raw_passthrough(uint32_t id,
                                          RpcSource source,
                                          uint32_t now) {
    expire_raw_passthrough(now);

    RawPassthroughRequest *slot = nullptr;
    for (auto &request : raw_passthrough_) {
        if (request.active && request.id == id) {
            slot = &request;
            break;
        }
        if (!request.active && !slot) slot = &request;
    }
    if (!slot) {
        slot = &raw_passthrough_[0];
        for (auto &request : raw_passthrough_) {
            if (static_cast<int32_t>(request.deadline_ms -
                                     slot->deadline_ms) < 0) {
                slot = &request;
            }
        }
    }

    slot->active = true;
    slot->id = id;
    slot->source = source;
    slot->deadline_ms = now + AC_RESMED_OTA_VERIFY_TIMEOUT_MS;
}

bool RpcArbiter::match_raw_passthrough(uint32_t id,
                                       RpcSource &source,
                                       uint32_t now) {
    expire_raw_passthrough(now);
    for (auto &request : raw_passthrough_) {
        if (!request.active || request.id != id) continue;
        source = request.source;
        request = {};
        return true;
    }
    return false;
}

void RpcArbiter::dispatch_next_request() {
    if (pending_.active) return;
    const uint32_t now = millis();
    if (dispatch_retry_active_ &&
        static_cast<int32_t>(now - next_dispatch_retry_ms_) < 0) {
        return;
    }
    if (static_cast<int32_t>(now - last_integrated_tx_ms_) <
        static_cast<int32_t>(AC_RPC_MIN_TX_INTERVAL_MS)) {
        return;
    }

    QueuedRequest request;
    if (dispatch_retry_active_) {
        request = dispatch_retry_;
    } else if (!requests_.pop(request)) {
        return;
    }

    if (request.set_datetime_now) {
        const std::string utc = current_utc_iso_nearest_second();
        if (utc.empty()) {
            cancel_queued_request(request, "datetime_clock_invalid");
            return;
        }
        request.params_json = build_set_datetime_params(utc);
    }

    const std::string payload = build_rpc_request(request.method,
                                                 request.params_json,
                                                 request.id);
    const auto frames = encode_datagram(payload);
    if (frames.size() > can_.tx_queue_free()) {
        if (!dispatch_retry_active_) {
            dispatch_retry_ = request;
            dispatch_retry_active_ = true;
            dispatch_retry_deadline_ms_ = now + request.timeout_ms;
        }
        next_dispatch_retry_ms_ = now + AC_RPC_MIN_TX_INTERVAL_MS;
        stats_.request_dispatch_retries++;
        if (static_cast<int32_t>(now - dispatch_retry_deadline_ms_) >= 0) {
            cancel_queued_request(dispatch_retry_, "dispatch_tx_full");
            dispatch_retry_ = {};
            dispatch_retry_active_ = false;
            dispatch_retry_deadline_ms_ = 0;
            next_dispatch_retry_ms_ = 0;
        }
        return;
    }

    if (!enqueue_encoded_frames(frames)) {
        if (!dispatch_retry_active_) {
            dispatch_retry_ = request;
            dispatch_retry_active_ = true;
            dispatch_retry_deadline_ms_ = now + request.timeout_ms;
        }
        next_dispatch_retry_ms_ = now + AC_RPC_MIN_TX_INTERVAL_MS;
        stats_.request_dispatch_retries++;
        if (static_cast<int32_t>(now - dispatch_retry_deadline_ms_) >= 0) {
            cancel_queued_request(dispatch_retry_, "dispatch_enqueue_failed");
            dispatch_retry_ = {};
            dispatch_retry_active_ = false;
            dispatch_retry_deadline_ms_ = 0;
            next_dispatch_retry_ms_ = 0;
        }
        return;
    }

    if (dispatch_retry_active_) {
        dispatch_retry_ = {};
        dispatch_retry_active_ = false;
        dispatch_retry_deadline_ms_ = 0;
        next_dispatch_retry_ms_ = 0;
    }

    pending_.active = true;
    pending_.id = request.id;
    pending_.source = request.source;
    pending_.method = request.method;
    pending_.stream_command = request.stream_command;
    pending_.settings_refresh = request.settings_refresh;
    pending_.deadline_ms = millis() + request.timeout_ms;
    pending_.dispatch_epoch_ms = 0;
    (void)current_epoch_ms(pending_.dispatch_epoch_ms);
    last_integrated_tx_ms_ = millis();
    stats_.dispatched_requests++;
    as11_state_.mark_therapy_command_sent(request.method,
                                          last_integrated_tx_ms_);
    if (request.method == "Set") {
        as11_settings_.note_set_request(request.params_json,
                                        last_integrated_tx_ms_);
    }

    Log::logf(CAT_RPC, LOG_DEBUG, "[RPC] dispatched id=%lu method=%s src=%s\n",
              static_cast<unsigned long>(request.id),
              request.method.c_str(),
              source_name(request.source));
    char prefix[112];
    snprintf(prefix, sizeof(prefix),
             "[RPC request id=%lu method=%s source=%s] ",
             static_cast<unsigned long>(request.id),
             request.method.c_str(),
             source_name(request.source));
    Log::log_payload(CAT_RPC, LOG_DEBUG, prefix, payload);
}

void RpcArbiter::check_pending_timeout() {
    if (!pending_.active) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - pending_.deadline_ms) < 0) return;

    char buf[128];
    snprintf(buf, sizeof(buf), "RPC request timeout id=%lu method=%s source=%s",
             static_cast<unsigned long>(pending_.id),
             pending_.method.c_str(),
             source_name(pending_.source));
    if (pending_.source == RpcSource::ResmedOta) {
        push_resmed_ota_event(RpcEventKind::Info, buf);
    } else if (pending_.source != RpcSource::Scheduler) {
        push_event(RpcEventKind::Info, buf);
    }
    stats_.request_timeouts++;
    note_request_timeout(pending_.source, now);
    if (pending_.stream_command != StreamCommandType::None) {
        stream_.mark_command_timeout(now);
    }
    if (pending_.method == "SubscribeEvent") {
        event_subscription_active_ = false;
        event_subscription_id_ = 0;
        next_event_subscribe_ms_ = now + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
        stats_.event_subscribe_errors++;
    }
    as11_state_.mark_therapy_command_timeout(pending_.method, now);
    if (pending_.method == "Set") {
        as11_settings_.note_set_cancelled("timeout", now);
    }
    pending_ = {};
}

void RpcArbiter::poll_event_subscription() {
    const uint32_t now = millis();
    if (event_subscription_active_) return;
    if (background_polls_suspended_) return;
    if (background_backoff_active(now)) return;

    if (next_event_subscribe_ms_ == 0) {
        next_event_subscribe_ms_ = now + AC_AS11_EVENT_SUBSCRIBE_DELAY_MS;
        return;
    }
    if (static_cast<int32_t>(now - next_event_subscribe_ms_) < 0) return;
    if (pending_.active || dispatch_retry_active_ || !requests_.empty()) {
        return;
    }

    QueuedRequest request;
    request.method = "SubscribeEvent";
    request.params_json = AS11_EVENT_SUBSCRIBE_PARAMS;
    request.source = RpcSource::Scheduler;
    request.timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
    if (enqueue_request(request)) {
        next_event_subscribe_ms_ = now + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
    } else {
        next_event_subscribe_ms_ = now + AC_RPC_DEFAULT_TIMEOUT_MS;
    }
}

void RpcArbiter::poll_stream_subscription() {
    const uint32_t now = millis();
    StreamCommand command =
        stream_.next_command(now, AC_STREAM_RESYNC_INTERVAL_MS);
    if (command.type == StreamCommandType::None) return;

    QueuedRequest request;
    request.method = "StartStream";
    request.params_json = command.params_json;
    request.source = RpcSource::Internal;
    request.timeout_ms = AC_RPC_STREAM_TIMEOUT_MS;
    request.stream_command = command.type;

    if (!enqueue_request(request)) {
        stream_.mark_command_deferred(now);
        stats_.stream_command_deferred++;
        return;
    }

    stream_.mark_command_queued(command.type, now);
    if (command.type == StreamCommandType::Start) {
        stats_.stream_start_requests++;
    } else if (command.type == StreamCommandType::Stop) {
        stats_.stream_stop_requests++;
    }
}

void RpcArbiter::poll_as11_healthcheck() {
    const uint32_t now = millis();
    as11_state_.poll(now);

    if (background_polls_suspended_) return;
    if (background_backoff_active(now)) return;

    if (!as11_healthcheck_initialized_) {
        as11_healthcheck_initialized_ = true;
        schedule_as11_identity_refresh(now,
                                       AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_status_refresh(now,
                                     AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_motor_refresh(now,
                                    AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
                                    AC_RPC_MIN_TX_INTERVAL_MS);
        schedule_as11_timezone_refresh(now,
                                       AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
                                       (AC_RPC_MIN_TX_INTERVAL_MS * 2));
        next_as11_clock_poll_ms_ = now + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
            AC_RPC_DEFAULT_TIMEOUT_MS;
        return;
    }

    if (pending_.active || dispatch_retry_active_ || !requests_.empty()) {
        return;
    }

    if (next_as11_identity_poll_ms_ &&
        static_cast<int32_t>(now - next_as11_identity_poll_ms_) >= 0) {
        QueuedRequest request;
        request.method = "Get";
        request.params_json = as11_identity_get_params_json();
        request.source = RpcSource::Scheduler;
        if (enqueue_request(request)) {
            next_as11_identity_poll_ms_ = 0;
        } else {
            next_as11_identity_poll_ms_ = now + AC_RPC_DEFAULT_TIMEOUT_MS;
        }
        return;
    }

    if (next_as11_status_poll_ms_ &&
        static_cast<int32_t>(now - next_as11_status_poll_ms_) >= 0) {
        QueuedRequest request;
        request.method = "Get";
        request.params_json = as11_runtime_get_params_json();
        request.source = RpcSource::Scheduler;
        if (enqueue_request(request)) {
            next_as11_status_poll_ms_ = now +
                (as11_state_.therapy_command_pending()
                     ? AC_AS11_THERAPY_STATUS_POLL_INTERVAL_MS
                     : AC_AS11_STATUS_POLL_INTERVAL_MS);
        } else {
            next_as11_status_poll_ms_ = now + AC_RPC_DEFAULT_TIMEOUT_MS;
        }
        return;
    }

    if (next_as11_motor_poll_ms_ &&
        static_cast<int32_t>(now - next_as11_motor_poll_ms_) >= 0) {
        QueuedRequest request;
        request.method = "Get";
        request.params_json = as11_motor_runtime_get_params_json();
        request.source = RpcSource::Scheduler;
        if (enqueue_request(request)) {
            next_as11_motor_poll_ms_ =
                as11_state_.therapy_state() == As11TherapyState::Running
                    ? now + AC_AS11_MOTOR_RUNTIME_POLL_INTERVAL_MS
                    : 0;
        } else {
            next_as11_motor_poll_ms_ = now + AC_RPC_DEFAULT_TIMEOUT_MS;
        }
        return;
    }

    if (next_as11_timezone_poll_ms_ &&
        static_cast<int32_t>(now - next_as11_timezone_poll_ms_) >= 0) {
        QueuedRequest request;
        request.method = "Get";
        request.params_json = as11_timezone_get_params_json();
        request.source = RpcSource::Scheduler;
        if (enqueue_request(request)) {
            next_as11_timezone_poll_ms_ =
                now + AC_AS11_TIMEZONE_POLL_INTERVAL_MS;
        } else {
            next_as11_timezone_poll_ms_ = now + AC_RPC_DEFAULT_TIMEOUT_MS;
        }
        return;
    }

    if (next_as11_clock_poll_ms_ &&
        static_cast<int32_t>(now - next_as11_clock_poll_ms_) >= 0) {
        QueuedRequest request;
        request.method = "GetDateTime";
        request.source = RpcSource::Scheduler;
        if (enqueue_request(request)) {
            next_as11_clock_poll_ms_ =
                now + AC_AS11_CLOCK_POLL_INTERVAL_MS;
        } else {
            next_as11_clock_poll_ms_ = now + AC_RPC_DEFAULT_TIMEOUT_MS;
        }
    }
}

void RpcArbiter::handle_matched_response(const std::string &payload) {
    const uint32_t now = millis();
    const bool is_error = json_member_present(payload, "error");
    const bool therapy_method =
        As11DeviceState::is_therapy_command_method(pending_.method);
    bool get_had_pending_therapy = false;
    bool settings_updated = false;
    if (pending_.method == "Set") {
        as11_settings_.note_set_response(is_error, now);
    }
    if (!is_error) {
        if (pending_.method == "Get") {
            get_had_pending_therapy = as11_state_.therapy_command_pending();
            const As11TherapyState before_get_state =
                as11_state_.therapy_state();
            as11_state_.apply_status_get_response(payload, now);
            settings_updated =
                as11_settings_.apply_settings_get_response(payload, now);
            if (before_get_state == As11TherapyState::Running &&
                as11_state_.therapy_state() == As11TherapyState::Standby) {
                schedule_as11_motor_refresh(
                    now, AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
            }
            update_as11_motor_refresh_after_state(now);
        } else if (pending_.method == "GetDateTime") {
            int64_t response_epoch_ms = 0;
            (void)current_epoch_ms(response_epoch_ms);
            as11_state_.apply_datetime_response(
                payload, now, pending_.dispatch_epoch_ms, response_epoch_ms);
        } else if (pending_.method == "SubscribeEvent") {
            uint32_t subscription_id = 0;
            if (as11_state_.apply_activity_subscription_response(
                    payload, now, subscription_id)) {
                event_subscription_active_ = true;
                event_subscription_id_ = subscription_id;
                next_event_subscribe_ms_ = 0;
                Log::logf(CAT_RPC, LOG_INFO,
                          "[RPC] subscribed to activity events id=%lu\n",
                          static_cast<unsigned long>(subscription_id));
            } else {
                event_subscription_active_ = false;
                event_subscription_id_ = 0;
                next_event_subscribe_ms_ =
                    now + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
                stats_.event_subscribe_errors++;
                Log::logf(CAT_RPC, LOG_WARN,
                          "[RPC] activity event subscription rejected\n");
            }
        }
    } else if (pending_.method == "SubscribeEvent") {
        event_subscription_active_ = false;
        event_subscription_id_ = 0;
        next_event_subscribe_ms_ = now + AC_AS11_EVENT_SUBSCRIBE_RETRY_MS;
        stats_.event_subscribe_errors++;
    }
    as11_state_.mark_therapy_command_response(pending_.method,
                                              is_error,
                                              now);
    if (!is_error && therapy_method) {
        schedule_as11_status_refresh(now,
                                     AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
    } else if (!is_error && pending_.method == "Get" &&
               get_had_pending_therapy) {
        if (as11_state_.therapy_command_pending()) {
            schedule_as11_status_refresh(
                now, AC_AS11_THERAPY_STATUS_POLL_INTERVAL_MS);
        } else {
            next_as11_status_poll_ms_ =
                now + AC_AS11_STATUS_POLL_INTERVAL_MS;
        }
    }
    if (pending_.stream_command != StreamCommandType::None) {
        stream_.mark_command_response(pending_.stream_command,
                                      is_error, now);
        if (is_error) stats_.stream_command_errors++;
    }
    if (settings_updated && pending_.settings_refresh) {
        push_event(RpcEventKind::InternalSettingsStateUpdated, "",
                   pending_.source, pending_.id);
    }
}

bool RpcArbiter::request_as11_healthcheck() {
    QueuedRequest identity;
    identity.method = "Get";
    identity.params_json = as11_identity_get_params_json();
    identity.source = RpcSource::Scheduler;
    if (!enqueue_request(identity)) return false;

    QueuedRequest status;
    status.method = "Get";
    status.params_json = as11_runtime_get_params_json();
    status.source = RpcSource::Scheduler;
    if (!enqueue_request(status)) return false;

    QueuedRequest motor;
    motor.method = "Get";
    motor.params_json = as11_motor_runtime_get_params_json();
    motor.source = RpcSource::Scheduler;
    if (!enqueue_request(motor)) return false;

    QueuedRequest timezone;
    timezone.method = "Get";
    timezone.params_json = as11_timezone_get_params_json();
    timezone.source = RpcSource::Scheduler;
    if (!enqueue_request(timezone)) return false;

    QueuedRequest clock;
    clock.method = "GetDateTime";
    clock.source = RpcSource::Scheduler;
    if (!enqueue_request(clock)) return false;

    const uint32_t now = millis();
    as11_healthcheck_initialized_ = true;
    next_as11_identity_poll_ms_ = 0;
    next_as11_status_poll_ms_ = now + AC_AS11_STATUS_POLL_INTERVAL_MS;
    next_as11_motor_poll_ms_ =
        as11_state_.therapy_state() == As11TherapyState::Running
            ? now + AC_AS11_MOTOR_RUNTIME_POLL_INTERVAL_MS
            : 0;
    next_as11_timezone_poll_ms_ = now + AC_AS11_TIMEZONE_POLL_INTERVAL_MS;
    next_as11_clock_poll_ms_ = now + AC_AS11_CLOCK_POLL_INTERVAL_MS;
    return true;
}

bool RpcArbiter::request_as11_settings_refresh() {
    QueuedRequest request;
    request.method = "Get";
    int mode = as11_settings_.mode_index();
    if (mode < 0) {
        mode = as11_mode_index_from_value(
            as11_state_.active_therapy_profile());
    }
    request.params_json = as11_settings_get_params_json(mode);
    request.source = RpcSource::Scheduler;
    request.settings_refresh = true;
    return enqueue_request(request);
}

bool RpcArbiter::handle_event_notification(const std::string &payload) {
    if (!json_method_is(payload, "EventNotification")) return false;
    const uint32_t now = millis();
    stats_.event_notifications++;
    const As11TherapyState before_state = as11_state_.therapy_state();
    const bool activity_updated =
        as11_state_.apply_activity_event_notification(payload, now);
    if (settings_history_change_notification(payload)) {
        push_event(RpcEventKind::InternalSettingsStateInvalidated, "",
                   RpcSource::Scheduler);
        request_as11_settings_refresh();
    }
    const std::string event = as11_state_.last_activity_event();
    if (activity_updated && event_suggests_identity_refresh(event)) {
        schedule_as11_identity_refresh(
            now, AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
        schedule_as11_status_refresh(now, AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
        schedule_as11_motor_refresh(now, AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
        schedule_as11_timezone_refresh(
            now, AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
    }
    if (activity_updated &&
        (as11_state_.therapy_state() != before_state ||
         event_suggests_status_refresh(event))) {
        stats_.activity_state_events++;
        schedule_as11_status_refresh(now, AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
    }
    if (activity_updated && event_suggests_motor_refresh(event) &&
        as11_state_.therapy_state() != As11TherapyState::Running) {
        schedule_as11_motor_refresh(now, AC_AS11_THERAPY_STATUS_POLL_DELAY_MS);
    }
    return true;
}

void RpcArbiter::handle_stream_notification(const std::string &payload) {
    if (!json_method_is(payload, "StreamData")) return;
    stats_.stream_notifications++;
    StreamPublishResult result =
        stream_.publish_stream_data(payload, millis());
    stats_.stream_fanout_drops += result.drops;
    if (result.parse_error) stats_.stream_parse_errors++;
    if (result.pool_exhausted) stats_.stream_pool_exhaustions++;
    if (result.raw_truncated || result.values_truncated) {
        stats_.stream_truncated_frames++;
    }
}

void RpcArbiter::handle_frame(const RawCanFrame &frame) {
    if (frame.extended || frame.remote) return;
    const uint32_t now = millis();

    if (frame.id == AC_CAN_RX_ID) {
        DatagramFeedResult result = rpc_rx_.feed(frame.data, frame.len, now);
        if (result.status == DatagramStatus::Complete) {
            handle_rpc_payload(result.payload);
        } else if (result.status == DatagramStatus::Error) {
            stats_.rpc_framing_errors++;
            push_event(RpcEventKind::FramingError,
                       std::string("[RPC] ") + result.error);
        }
        return;
    }

    if (frame.id == AC_CAN_LOG_ID) {
        DatagramFeedResult result = log_rx_.feed(frame.data, frame.len, now);
        if (result.status == DatagramStatus::Complete) {
            handle_debug_payload(result.payload);
        } else if (result.status == DatagramStatus::Error) {
            stats_.log_framing_errors++;
            push_event(RpcEventKind::FramingError,
                       std::string("[LOG] ") + result.error);
        }
        return;
    }

    if (frame.id == AC_CAN_BOOT_ID) {
        stats_.boot_notifications++;
        boot_notifications_seen_++;
        last_boot_notification_ = format_boot_frame(frame);
        last_boot_notification_ms_ = millis();
        cancel_all_requests("device_boot");
        as11_state_.reset();
        as11_settings_.clear();
        event_subscription_active_ = false;
        event_subscription_id_ = 0;
        next_event_subscribe_ms_ = now + AC_AS11_EVENT_SUBSCRIBE_DELAY_MS;
        as11_healthcheck_initialized_ = true;
        schedule_as11_identity_refresh(now,
                                       AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_status_refresh(now, AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_motor_refresh(now, AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_timezone_refresh(now,
                                       AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        next_as11_clock_poll_ms_ = now + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
            AC_RPC_DEFAULT_TIMEOUT_MS;
        if (stream_.desired_active()) stream_.mark_reattach();
        push_event(RpcEventKind::BootNotification, last_boot_notification_);
    }
}

void RpcArbiter::handle_rpc_payload(const std::string &payload) {
    stats_.rpc_datagrams++;
    switch (classify_rpc_payload(payload)) {
        case RpcPayloadKind::Notification: {
            stats_.rpc_notifications++;
            handle_stream_notification(payload);
            handle_event_notification(payload);
            if (!json_method_is(payload, "StreamData")) {
                Log::log_payload(CAT_RPC, LOG_DEBUG, "[RPC notify] ",
                                 payload);
            }
            push_event(RpcEventKind::RpcNotification, payload);
            break;
        }
        case RpcPayloadKind::Response: {
            stats_.rpc_responses++;
            uint32_t response_id = 0;
            const bool has_response_id = json_extract_id(payload, response_id);
            if (pending_.active && has_response_id &&
                response_id == pending_.id) {
                const uint32_t matched_id = pending_.id;
                const std::string matched_method = pending_.method;
                const RpcSource response_source = pending_.source;
                if (response_source != RpcSource::Console) {
                    char prefix[112];
                    snprintf(prefix, sizeof(prefix),
                             "[RPC response id=%lu method=%s source=%s] ",
                             static_cast<unsigned long>(matched_id),
                             matched_method.c_str(),
                             source_name(response_source));
                    Log::log_payload(CAT_RPC, LOG_DEBUG, prefix, payload);
                }
                handle_matched_response(payload);
                note_request_success(response_source, millis());
                pending_ = {};
                if (response_source == RpcSource::ResmedOta) {
                    push_resmed_ota_event(RpcEventKind::RpcResponse, payload,
                                          response_source, matched_id);
                    break;
                }
                if (!emit_matched_response(response_source)) break;
                push_event(RpcEventKind::RpcResponse, payload,
                           response_source, matched_id);
                break;
            }
            RpcSource passthrough_source = RpcSource::Internal;
            if (has_response_id &&
                match_raw_passthrough(response_id, passthrough_source,
                                      millis())) {
                if (Log::get_cat_level(CAT_RPC) >= LOG_DEBUG) {
                    char prefix[112];
                    snprintf(prefix, sizeof(prefix),
                             "[RPC response id=%lu source=%s] ",
                             static_cast<unsigned long>(response_id),
                             source_name(passthrough_source));
                    Log::log_payload(CAT_RPC, LOG_DEBUG, prefix, payload);
                }
                push_event(RpcEventKind::RpcResponse, payload,
                           passthrough_source, response_id);
                break;
            }
            if (pending_.active) {
                stats_.rpc_unmatched++;
                Log::logf(CAT_RPC, LOG_DEBUG,
                          "[RPC] response did not match pending id=%lu\n",
                          static_cast<unsigned long>(pending_.id));
            }
            Log::log_payload(CAT_RPC, LOG_DEBUG, "[RPC response unmatched] ",
                             payload);
            push_event(RpcEventKind::RpcResponse, payload);
            break;
        }
        case RpcPayloadKind::Unknown:
            stats_.rpc_unmatched++;
            Log::log_payload(CAT_RPC, LOG_DEBUG, "[RPC unmatched] ", payload);
            push_event(RpcEventKind::RpcUnmatched, payload);
            break;
    }
}

void RpcArbiter::handle_debug_payload(const std::string &payload) {
    stats_.log_datagrams++;
    Log::log_payload(CAT_RPC, LOG_DEBUG, "[AS11 log] ", payload);
    push_event(RpcEventKind::DebugLog, payload);
}

std::string RpcArbiter::format_boot_frame(const RawCanFrame &frame) const {
    std::string out = "FgPowerup 0x";
    char id[8];
    snprintf(id, sizeof(id), "%03lX", static_cast<unsigned long>(frame.id));
    out += id;
    out += " [";
    out += std::to_string(frame.len);
    out += "] ";
    out += hex_bytes(frame.data, frame.len);
    return out;
}

const char *RpcArbiter::source_name(RpcSource source) const {
    switch (source) {
        case RpcSource::Console: return "console";
        case RpcSource::Tcp: return "tcp";
        case RpcSource::HttpApi: return "http";
        case RpcSource::Scheduler: return "scheduler";
        case RpcSource::Internal: return "internal";
        case RpcSource::ResmedOta: return "resmed_ota";
        case RpcSource::Sink: return "sink";
        default: return "?";
    }
}

uint8_t RpcArbiter::source_id(RpcSource source) const {
    return static_cast<uint8_t>(source);
}

}  // namespace aircannect
