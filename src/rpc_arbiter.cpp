#include "rpc_arbiter.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "as11_rpc.h"
#include "debug_log.h"
#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr uint32_t AS11_SETTINGS_READBACK_TIMEOUT_MS = 20000;
constexpr uint32_t AS11_SETTINGS_REFRESH_RETRY_MS = 1000;

void *alloc_payload_bytes(size_t bytes) {
#ifdef ARDUINO
    return Memory::alloc_large(bytes);
#else
    return malloc(bytes);
#endif
}

void free_payload_bytes(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

bool emit_matched_response(RpcSource source) {
    return source == RpcSource::Console ||
           source == RpcSource::Tcp ||
           source == RpcSource::HttpApi;
}

bool scheduler_source(RpcSource source) {
    return source == RpcSource::Scheduler || source == RpcSource::Internal;
}

StreamCommandType raw_stream_command(const std::string &payload,
                                     std::string &params_json) {
    params_json.clear();
    if (!json_method_is(payload, "StartStream")) return StreamCommandType::None;

    JsonDocument doc;
    if (deserializeJson(doc, payload.c_str())) return StreamCommandType::Start;

    JsonVariant params = doc["params"];
    if (!params.isNull()) {
        String encoded;
        serializeJson(params, encoded);
        params_json = encoded.c_str();
    }

    JsonArray data_ids = params["dataIds"].as<JsonArray>();
    if (!data_ids.isNull() && data_ids.size() == 0) {
        return StreamCommandType::Stop;
    }
    return StreamCommandType::Start;
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

RpcArbiter::DeferredPayload::~DeferredPayload() {
    clear();
}

RpcArbiter::DeferredPayload::DeferredPayload(DeferredPayload &&other) noexcept {
    move_from(other);
}

RpcArbiter::DeferredPayload &RpcArbiter::DeferredPayload::operator=(
    DeferredPayload &&other) noexcept {
    if (this != &other) {
        clear();
        move_from(other);
    }
    return *this;
}

bool RpcArbiter::DeferredPayload::copy_from(Kind next_kind,
                                            const char *payload,
                                            size_t payload_len) {
    clear();
    kind = next_kind;
    if (!payload || payload_len == 0) return true;
    char *next = static_cast<char *>(alloc_payload_bytes(payload_len));
    if (!next) return false;
    memcpy(next, payload, payload_len);
    data = next;
    len = payload_len;
    return true;
}

void RpcArbiter::DeferredPayload::clear() {
    free_payload_bytes(data);
    data = nullptr;
    len = 0;
    kind = Kind::Rpc;
}

void RpcArbiter::DeferredPayload::move_from(DeferredPayload &other) {
    kind = other.kind;
    data = other.data;
    len = other.len;
    other.kind = Kind::Rpc;
    other.data = nullptr;
    other.len = 0;
}

bool RpcArbiter::reserve_reassembly_buffers() {
    const bool rpc_ok = rpc_rx_.reserve_initial();
    const bool log_ok = log_rx_.reserve_initial();
    return rpc_ok && log_ok;
}

void RpcArbiter::poll() {
    can_.poll();
    const uint32_t now = millis();
    note_can_rx_pressure(now);
    if (esp_ota_quiesce_requested_ &&
        esp_ota_quiesce_deadline_ms_ &&
        !esp_ota_quiesce_complete() &&
        !esp_ota_quiesce_timeout_logged_ &&
        static_cast<int32_t>(now - esp_ota_quiesce_deadline_ms_) >= 0) {
        esp_ota_quiesce_timeout_logged_ = true;
        const EventBrokerStatus event_status = event_.status();
        Log::logf(CAT_OTA, LOG_WARN,
                  "AS11 quiesce timed out stream=%u event=%u pending=%u "
                  "retry=%u queue=%u tx_q=%u event_active=%u "
                  "event_pending=%u\n",
                  stream_.quiesced() ? 1u : 0u,
                  event_.quiesced() ? 1u : 0u,
                  pending_.active ? 1u : 0u,
                  dispatch_retry_active_ ? 1u : 0u,
                  static_cast<unsigned>(requests_.count()),
                  static_cast<unsigned>(can_.tx_queue_depth()),
                  event_status.subscription_active ? 1u : 0u,
                  event_status.subscribe_pending ? 1u : 0u);
    }

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

    drain_can_rx();
    process_deferred_payloads(AC_RPC_PAYLOAD_DRAIN_BUDGET);

    check_pending_timeout();
    poll_as11_settings_refresh(millis());
    poll_stream_subscription();
    poll_event_subscription();
    poll_as11_healthcheck();
    dispatch_next_request();

    // Dispatch may enqueue CAN frames after RX-driven work above. Poll again so
    // the CAN driver can start TX in the same main-loop turn.
    can_.poll();
}

size_t RpcArbiter::drain_can_rx() {
    size_t drained = 0;
    const uint32_t start_ms = millis();
    for (; drained < AC_CAN_RX_DRAIN_PRESSURE_BUDGET; ++drained) {
        RawCanFrame frame;
        if (!can_.receive(frame, 0)) break;
        handle_frame(frame);
        if (drained + 1 < AC_CAN_RX_DRAIN_BASE_BUDGET) continue;
        if (!can_rx_queue_pressure_active()) break;
        if (millis() - start_ms >= AC_CAN_RX_DRAIN_PRESSURE_MAX_MS) break;
    }
    return drained;
}

void RpcArbiter::process_deferred_payloads(size_t budget) {
    for (size_t i = 0; i < budget; ++i) {
        DeferredPayload payload;
        if (!deferred_payloads_.pop(payload)) return;
        if (payload.kind == DeferredPayload::Kind::Rpc) {
            handle_rpc_payload(payload.data, payload.len);
        } else {
            handle_debug_payload(payload.data, payload.len);
        }
        drain_can_rx();
    }
}

bool RpcArbiter::submit_raw_payload(const std::string &payload, RpcSource source) {
    if (esp_ota_quiesce_requested_) {
        push_event(RpcEventKind::Info,
                   "raw RPC rejected while ESP OTA quiesce is active");
        return false;
    }
    if (Log::get_cat_level(CAT_RPC) >= LOG_DEBUG) {
        char prefix[80];
        snprintf(prefix, sizeof(prefix), "[RAW request source=%s] ",
                 source_name(source));
        Log::log_payload(CAT_RPC, LOG_DEBUG, prefix, payload);
    }
    std::string stream_params;
    const StreamCommandType stream_command =
        (source == RpcSource::Console || source == RpcSource::Tcp)
            ? raw_stream_command(payload, stream_params)
            : StreamCommandType::None;
    if (!enqueue_payload_frames(payload, source)) return false;

    uint32_t id = 0;
    if ((source == RpcSource::Console || source == RpcSource::Tcp) &&
        json_extract_id(payload, id)) {
        remember_raw_passthrough(id, source, stream_command, millis());
    }
    if (source == RpcSource::Tcp) {
        note_raw_stream_request(stream_command, stream_params);
    }
    return true;
}

bool RpcArbiter::enqueue_payload_frames(const std::string &payload,
                                        RpcSource source) {
    (void)source;
    const size_t frame_count = datagram_frame_count(payload.size());
    if (frame_count > can_.tx_queue_free()) {
        push_event(RpcEventKind::Info, "CAN TX queue full; payload rejected");
        return false;
    }

    return visit_encoded_datagram(payload, enqueue_datagram_frame, this);
}

bool RpcArbiter::enqueue_datagram_frame(void *context,
                                        const DatagramFrame &frame) {
    RpcArbiter *arbiter = static_cast<RpcArbiter *>(context);
    if (!arbiter) return false;

    RawCanFrame raw;
    raw.id = AC_CAN_TX_ID;
    raw.len = frame.len;
    for (uint8_t i = 0; i < frame.len; ++i) raw.data[i] = frame.data[i];
    if (!arbiter->can_.enqueue_tx(raw)) {
        arbiter->push_event(RpcEventKind::Info, "CAN TX enqueue failed");
        return false;
    }

    return true;
}

bool RpcArbiter::send_request(const std::string &method,
                              const std::string &params_json,
                              RpcSource source,
                              uint32_t timeout_ms) {
    uint32_t id = 0;
    return send_request_with_id(method, params_json, source, timeout_ms, id);
}

bool RpcArbiter::send_request_with_id(const std::string &method,
                                      const std::string &params_json,
                                      RpcSource source,
                                      uint32_t timeout_ms,
                                      uint32_t &id) {
    QueuedRequest request;
    request.method = method;
    request.params_json = params_json;
    request.source = source;
    request.timeout_ms = timeout_ms ? timeout_ms :
        ((method == "StartStream") ? AC_RPC_STREAM_TIMEOUT_MS
                                   : AC_RPC_DEFAULT_TIMEOUT_MS);

    const bool queued = enqueue_request(request);
    id = queued ? request.id : 0;
    return queued;
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
    const bool ota_quiesce_control =
        esp_ota_quiesce_requested_ &&
        request_allowed_during_esp_ota_quiesce(request);
    if (scheduler_source(request.source) && background_backoff_active(now) &&
        !ota_quiesce_control) {
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

bool RpcArbiter::next_source_event(RpcSource source, RpcEvent &event) {
    const SourceEventRoute *route = source_event_route(source);
    if (!route) return false;
    return pop_source_event_queue(route->queue, event);
}

bool RpcArbiter::background_backpressure_active() const {
    if (background_rx_pressure_active(millis())) return true;

    if (can_rx_queue_pressure_active()) return true;
    if (deferred_payloads_.count() >=
        AC_RPC_PAYLOAD_BACKPRESSURE_WATERMARK) {
        return true;
    }
    if (report_events_.count() >= AC_REPORT_EVENT_BACKPRESSURE_WATERMARK) {
        return true;
    }
    return false;
}

bool RpcArbiter::can_rx_queue_pressure_active() const {
    CanControllerStatus can_status;
    return can_.controller_status(can_status) && can_status.valid &&
           can_status.msgs_to_rx >= AC_CAN_RX_BACKPRESSURE_WATERMARK;
}

bool RpcArbiter::set_source_event_observer(RpcSource source,
                                           RpcEventObserver observer,
                                           void *context) {
    SourceEventRoute *route = source_event_route(source);
    if (!route) return false;
    route->observer = observer;
    route->observer_context = observer ? context : nullptr;
    return true;
}

void RpcArbiter::set_raw_rpc_events_enabled(bool enabled) {
    if (raw_rpc_events_enabled_ && !enabled) {
        stream_.note_external_stop(millis(),
                                   ExternalStreamStopMode::CommandRequired);
    }
    raw_rpc_events_enabled_ = enabled;
}

bool RpcArbiter::add_event_frame_observer(EventFrameObserver observer,
                                          void *context) {
    return event_.add_frame_observer(observer, context);
}

EventAcquireResult RpcArbiter::acquire_events(const char *data_ids_csv) {
    return event_.acquire(data_ids_csv);
}

void RpcArbiter::release_events(EventConsumerHandle handle) {
    event_.release(handle);
}

bool RpcArbiter::event_consumer_active(EventConsumerHandle handle) const {
    return event_.consumer_active(handle);
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
    if (stream_realtime_active()) return true;
    return stream_.last_owned_activity_ms() &&
           millis() - stream_.last_owned_activity_ms() <
               AC_WIFI_ROAM_STREAM_QUIET_MS;
}

bool RpcArbiter::stream_realtime_active() const {
    return stream_.desired_active() || stream_.actual_active() ||
           stream_.pending();
}

bool RpcArbiter::stream_actual_active() const {
    return stream_.actual_active();
}

bool RpcArbiter::stream_accepted_data_ids_cover(
    const char *data_ids_csv) const {
    return stream_.accepted_data_ids_cover(data_ids_csv);
}

const std::string &RpcArbiter::stream_accepted_data_ids_csv() const {
    return stream_.accepted_data_ids_csv();
}

void RpcArbiter::set_stream_frame_observer(StreamFrameObserver observer,
                                           void *context) {
    stream_.set_frame_observer(observer, context);
}

bool RpcArbiter::next_stream_frame(StreamConsumerHandle handle,
                                   StreamFrameRef &frame) {
    return stream_.next_frame(handle, frame);
}

void RpcArbiter::reset_stats() {
    stats_ = {};
    can_.reset_stats();
    event_.reset_counters();
    stream_.reset_counters();
    consecutive_scheduler_timeouts_ = 0;
    background_backoff_until_ms_ = 0;
    background_rx_pressure_until_ms_ = 0;
    observed_rx_queue_full_alerts_ = 0;
    stats_started_ms_ = millis();
}

RpcRuntimeStatus RpcArbiter::runtime_status() const {
    const uint32_t now = millis();
    RpcRuntimeStatus out;
    out.stats_elapsed_ms = std::max<uint32_t>(1, now - stats_started_ms_);
    out.request_queue_depth = requests_.count();
    out.payload_queue_depth = deferred_payloads_.count();
    out.pending_request_id = pending_.active ? pending_.id : 0;
    out.dispatch_retry_id = dispatch_retry_active_ ? dispatch_retry_.id : 0;
    const uint32_t timeout_backoff_ms = background_backoff_active(now)
        ? background_backoff_until_ms_ - now
        : 0;
    const uint32_t rx_pressure_backoff_ms = background_rx_pressure_active(now)
        ? background_rx_pressure_until_ms_ - now
        : 0;
    out.background_backoff_ms =
        std::max(timeout_backoff_ms, rx_pressure_backoff_ms);
    const EventBrokerStatus event_status = event_.status();
    out.event_subscription_active = event_status.subscription_active;
    out.event_subscription_id = event_status.subscription_id;
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
    deferred_payloads_.clear();
    cancel_all_requests(reason ? reason : "can_recovery");
    const uint32_t now = millis();
    stream_.mark_reattach(now);
    event_.mark_reattach(now);
    if (as11_state_.therapy_command_pending()) {
        as11_state_.clear_pending_therapy_command("can_recovery", now);
    }
    return can_.recover_or_restart(reason);
}

void RpcArbiter::set_background_polls_suspended(bool suspended) {
    background_polls_suspended_ = suspended;
}

void RpcArbiter::set_esp_ota_quiesce(bool requested) {
    const uint32_t now = millis();
    if (requested == esp_ota_quiesce_requested_) return;

    esp_ota_quiesce_requested_ = requested;
    esp_ota_quiesce_timeout_logged_ = false;
    if (!requested) {
        esp_ota_quiesce_deadline_ms_ = 0;
        stream_.clear_quiesce();
        event_.clear_quiesce(now);
        return;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "quiescing AS11 push traffic before ESP OTA\n");
    cancel_all_requests("esp_ota");
    stream_.request_quiesce(now);
    event_.request_quiesce(now);
    esp_ota_quiesce_deadline_ms_ =
        now + AC_ESP_OTA_QUIESCE_TIMEOUT_MS;
}

bool RpcArbiter::esp_ota_quiesce_complete() const {
    if (!esp_ota_quiesce_requested_) return true;
    return stream_.quiesced() && event_.quiesced() &&
           !pending_.active && !dispatch_retry_active_ && requests_.empty() &&
           can_.tx_queue_depth() == 0;
}

bool RpcArbiter::esp_ota_quiesce_timed_out() const {
    if (!esp_ota_quiesce_requested_ || !esp_ota_quiesce_deadline_ms_) {
        return false;
    }
    const uint32_t now = millis();
    return static_cast<int32_t>(now - esp_ota_quiesce_deadline_ms_) >= 0;
}

bool RpcArbiter::esp_ota_reboot_allowed() const {
    return esp_ota_quiesce_complete() || esp_ota_quiesce_timed_out();
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

bool RpcArbiter::background_rx_pressure_active(uint32_t now) const {
    return background_rx_pressure_until_ms_ &&
           static_cast<int32_t>(background_rx_pressure_until_ms_ - now) > 0;
}

void RpcArbiter::note_can_rx_pressure(uint32_t now) {
    const uint32_t alerts = can_.stats().rx_queue_full_alerts;
    if (alerts == observed_rx_queue_full_alerts_) return;
    observed_rx_queue_full_alerts_ = alerts;

    const uint32_t until =
        now + AC_RPC_BACKGROUND_RX_PRESSURE_BACKOFF_MS;
    if (!background_rx_pressure_until_ms_ ||
        static_cast<int32_t>(until - background_rx_pressure_until_ms_) > 0) {
        background_rx_pressure_until_ms_ = until;
    }
}

bool RpcArbiter::request_allowed_during_esp_ota_quiesce(
    const QueuedRequest &request) const {
    if (!esp_ota_quiesce_requested_) return true;
    if (request.method == "StartStream" &&
        request.stream_command == StreamCommandType::Stop) {
        return true;
    }
    if (request.method == "SubscribeEvent" &&
        request.event_command == EventCommandType::Quiesce) {
        return true;
    }
    return false;
}

RpcArbiter::DateTimePrepareResult RpcArbiter::prepare_set_datetime_request(
    QueuedRequest &request, uint32_t now) {
    int64_t now_epoch_ms = 0;
    if (!current_epoch_ms(now_epoch_ms)) {
        return DateTimePrepareResult::InvalidClock;
    }

    const int64_t max_future_ms = 2000;
    const bool target_missing =
        request.set_datetime_target_epoch_ms < 1609459200000LL;
    const bool target_missed =
        request.set_datetime_target_epoch_ms <= now_epoch_ms;
    const bool target_from_old_clock =
        request.set_datetime_target_epoch_ms - now_epoch_ms > max_future_ms;
    if (target_missing || target_missed || target_from_old_clock) {
        int64_t target_epoch_ms = ((now_epoch_ms / 1000) + 1) * 1000;
        const int64_t remaining_ms = target_epoch_ms - now_epoch_ms;
        if (remaining_ms <=
            static_cast<int64_t>(AC_RPC_SET_DATETIME_APPLY_LEAD_MS +
                                 AC_RPC_SET_DATETIME_TARGET_MARGIN_MS)) {
            target_epoch_ms += 1000;
        }
        request.set_datetime_target_epoch_ms = target_epoch_ms;
    }

    const int64_t fire_epoch_ms =
        request.set_datetime_target_epoch_ms -
        static_cast<int64_t>(AC_RPC_SET_DATETIME_APPLY_LEAD_MS);
    if (now_epoch_ms < fire_epoch_ms) {
        const uint32_t wait_ms = static_cast<uint32_t>(
            std::min<int64_t>(fire_epoch_ms - now_epoch_ms, 1000));
        dispatch_retry_ = request;
        dispatch_retry_active_ = true;
        if (!dispatch_retry_deadline_ms_) {
            dispatch_retry_deadline_ms_ = now + wait_ms + request.timeout_ms;
        }
        next_dispatch_retry_ms_ = now + wait_ms;
        return DateTimePrepareResult::Deferred;
    }

    const std::string utc = format_utc_ms(request.set_datetime_target_epoch_ms);
    if (utc.empty()) return DateTimePrepareResult::InvalidClock;
    request.params_json = build_set_datetime_params(utc);
    return DateTimePrepareResult::Ready;
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
              "background polling paused for %lu ms after %u timeouts\n",
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
    if (payload.empty()) {
        push_event(kind, RpcPayloadRef(), source, id);
        return;
    }
    push_event(kind, make_rpc_payload_ref(std::string(payload)), source, id);
}

void RpcArbiter::push_event(RpcEventKind kind,
                            RpcPayloadRef payload,
                            RpcSource source,
                            uint32_t id) {
    RpcEvent event;
    event.kind = kind;
    event.source = source;
    event.id = id;
    event.payload = std::move(payload);
    if (!events_.push(std::move(event))) stats_.event_drops++;
}

void RpcArbiter::push_source_event(RpcSource target,
                                   RpcEventKind kind,
                                   const std::string &payload,
                                   RpcSource source,
                                   uint32_t id) {
    if (payload.empty()) {
        push_source_event(target, kind, RpcPayloadRef(), source, id);
        return;
    }
    push_source_event(target, kind, make_rpc_payload_ref(std::string(payload)),
                      source, id);
}

void RpcArbiter::push_source_event(RpcSource target,
                                   RpcEventKind kind,
                                   RpcPayloadRef payload,
                                   RpcSource source,
                                   uint32_t id) {
    const SourceEventRoute *route = source_event_route(target);
    if (!route) {
        stats_.event_drops++;
        return;
    }

    RpcEvent event;
    event.kind = kind;
    event.source = source;
    event.id = id;
    event.payload = std::move(payload);
    if (dispatch_source_event(*route, event)) return;
    if (push_source_event_queue(route->queue, std::move(event))) return;
    Log::logf(CAT_RPC,
              LOG_WARN,
              "source event queue full target=%s kind=%u\n",
              source_name(target),
              static_cast<unsigned>(kind));
    stats_.event_drops++;
}

RpcArbiter::SourceEventRoute *RpcArbiter::source_event_route(
    RpcSource source) {
    for (auto &route : source_event_routes_) {
        if (route.source == source) return &route;
    }
    return nullptr;
}

const RpcArbiter::SourceEventRoute *RpcArbiter::source_event_route(
    RpcSource source) const {
    for (const auto &route : source_event_routes_) {
        if (route.source == source) return &route;
    }
    return nullptr;
}

bool RpcArbiter::dispatch_source_event(const SourceEventRoute &route,
                                       const RpcEvent &event) {
    if (!route.observer) return false;
    route.observer(route.observer_context, event);
    return true;
}

bool RpcArbiter::push_source_event_queue(SourceEventQueue queue,
                                         RpcEvent &&event) {
    switch (queue) {
        case SourceEventQueue::Report:
            return report_events_.push(std::move(event));
        case SourceEventQueue::ResmedOta:
            return resmed_ota_events_.push(std::move(event));
        case SourceEventQueue::None:
        default:
            return false;
    }
}

bool RpcArbiter::pop_source_event_queue(SourceEventQueue queue,
                                        RpcEvent &event) {
    switch (queue) {
        case SourceEventQueue::Report:
            return report_events_.pop(event);
        case SourceEventQueue::ResmedOta:
            return resmed_ota_events_.pop(event);
        case SourceEventQueue::None:
        default:
            return false;
    }
}

void RpcArbiter::cancel_pending_request(const char *reason) {
    if (!pending_.active) return;
    if (pending_.settings_refresh) {
        finish_as11_settings_refresh();
        schedule_as11_settings_refresh_retry(RpcSource::Scheduler, millis());
    }

    stats_.request_cancellations++;
    if (pending_.source == RpcSource::ResmedOta) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(pending_.id),
                 pending_.method.c_str(),
                 source_name(pending_.source),
                 reason ? reason : "unknown");
        push_source_event(RpcSource::ResmedOta, RpcEventKind::Info, buf);
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
    if (pending_.event_command != EventCommandType::None) {
        event_.mark_command_cancelled(millis());
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
    if (request.settings_refresh) {
        finish_as11_settings_refresh();
        schedule_as11_settings_refresh_retry(RpcSource::Scheduler, millis());
    }

    stats_.request_cancellations++;
    if (request.source == RpcSource::ResmedOta) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(request.id),
                 request.method.c_str(),
                 source_name(request.source),
                 reason ? reason : "unknown");
        push_source_event(RpcSource::ResmedOta, RpcEventKind::Info, buf);
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
    if (request.event_command != EventCommandType::None) {
        event_.mark_command_cancelled(millis());
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
                                          StreamCommandType stream_command,
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
    slot->stream_command = stream_command;
    slot->deadline_ms = now + AC_RESMED_OTA_VERIFY_TIMEOUT_MS;
}

bool RpcArbiter::match_raw_passthrough(uint32_t id,
                                       RawPassthroughRequest &matched,
                                       uint32_t now) {
    expire_raw_passthrough(now);
    for (auto &request : raw_passthrough_) {
        if (!request.active || request.id != id) continue;
        matched = request;
        request = {};
        return true;
    }
    return false;
}

bool RpcArbiter::match_raw_passthrough(uint32_t id,
                                       RpcSource &source,
                                       uint32_t now) {
    RawPassthroughRequest request;
    if (!match_raw_passthrough(id, request, now)) return false;
    source = request.source;
    return true;
}

void RpcArbiter::note_raw_stream_request(StreamCommandType command,
                                         const std::string &params_json) {
    const uint32_t now = millis();
    if (command == StreamCommandType::Start) {
        stream_.note_external_start(params_json, now);
    } else if (command == StreamCommandType::Stop) {
        stream_.note_external_stop(now);
    }
}

void RpcArbiter::note_raw_stream_response(StreamCommandType command,
                                          bool is_error) {
    if (!is_error) return;
    if (command == StreamCommandType::Start) {
        stream_.note_external_stop(millis());
    }
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
    const bool from_retry = dispatch_retry_active_;
    if (dispatch_retry_active_) {
        request = dispatch_retry_;
    } else if (!requests_.pop(request)) {
        return;
    }

    if (esp_ota_quiesce_requested_ &&
        !request_allowed_during_esp_ota_quiesce(request)) {
        cancel_queued_request(request, "esp_ota");
        if (from_retry) {
            dispatch_retry_ = {};
            dispatch_retry_active_ = false;
            dispatch_retry_deadline_ms_ = 0;
            next_dispatch_retry_ms_ = 0;
        }
        return;
    }

    if (request.set_datetime_now) {
        const DateTimePrepareResult result =
            prepare_set_datetime_request(request, now);
        if (result == DateTimePrepareResult::Deferred) {
            return;
        }
        if (result == DateTimePrepareResult::InvalidClock) {
            cancel_queued_request(request, "datetime_clock_invalid");
            if (from_retry) {
                dispatch_retry_ = {};
                dispatch_retry_active_ = false;
                dispatch_retry_deadline_ms_ = 0;
                next_dispatch_retry_ms_ = 0;
            }
            return;
        }
    }

    const std::string payload = build_rpc_request(request.method,
                                                  request.params_json,
                                                  request.id);
    const size_t frame_count = datagram_frame_count(payload.size());
    if (frame_count > can_.tx_queue_free()) {
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

    if (!visit_encoded_datagram(payload, enqueue_datagram_frame, this)) {
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
    pending_.event_command = request.event_command;
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

    Log::logf(CAT_RPC, LOG_DEBUG, "dispatched id=%lu method=%s src=%s\n",
              static_cast<unsigned long>(request.id),
              request.method.c_str(),
              source_name(request.source));
    char prefix[112];
    snprintf(prefix, sizeof(prefix),
             "[REQUEST id=%lu method=%s source=%s] ",
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
        push_source_event(RpcSource::ResmedOta, RpcEventKind::Info, buf);
    } else if (pending_.source != RpcSource::Scheduler) {
        push_event(RpcEventKind::Info, buf);
    }
    stats_.request_timeouts++;
    const bool ota_quiesce_control =
        esp_ota_quiesce_requested_ &&
        ((pending_.method == "StartStream" &&
          pending_.stream_command == StreamCommandType::Stop) ||
         (pending_.method == "SubscribeEvent" &&
          pending_.event_command == EventCommandType::Quiesce));
    if (!ota_quiesce_control) {
        note_request_timeout(pending_.source, now);
    }
    if (pending_.stream_command != StreamCommandType::None) {
        stream_.mark_command_timeout(now);
    }
    if (pending_.event_command != EventCommandType::None) {
        event_.mark_command_timeout(now);
    }
    as11_state_.mark_therapy_command_timeout(pending_.method, now);
    if (pending_.method == "Set") {
        as11_settings_.note_set_cancelled("timeout", now);
    }
    if (pending_.settings_refresh) {
        finish_as11_settings_refresh();
        schedule_as11_settings_refresh_retry(RpcSource::Scheduler, now);
    }

    pending_ = {};
}

void RpcArbiter::poll_event_subscription() {
    const uint32_t now = millis();
    if (background_polls_suspended_ && !esp_ota_quiesce_requested_) return;
    if (background_backoff_active(now) && !esp_ota_quiesce_requested_) return;
    EventCommand command = event_.next_command(now);
    if (command.type == EventCommandType::None) return;
    if (pending_.active || dispatch_retry_active_ || !requests_.empty()) {
        return;
    }

    QueuedRequest request;
    request.method = "SubscribeEvent";
    request.params_json = command.params_json;
    request.source = RpcSource::Scheduler;
    request.timeout_ms = AC_RPC_DEFAULT_TIMEOUT_MS;
    request.event_command = command.type;
    if (enqueue_request(request)) {
        event_.mark_command_queued(command.type, command.params_json, now);
    } else {
        event_.mark_command_deferred(now);
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

    if (esp_ota_quiesce_requested_) return;
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
    bool settings_snapshot_complete = false;
    if (pending_.method == "Set") {
        as11_settings_.note_set_response(is_error, now);
    }
    if (!is_error) {
        if (pending_.method == "Get") {
            get_had_pending_therapy = as11_state_.therapy_command_pending();
            const As11TherapyState before_get_state =
                as11_state_.therapy_state();
            as11_state_.apply_status_get_response(payload, now);
            settings_updated = as11_settings_.apply_settings_get_response(
                payload, now, &settings_snapshot_complete);
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
        } else if (pending_.event_command != EventCommandType::None) {
            uint32_t subscription_id = 0;
            const bool subscribed =
                event_.accept_subscribe_response(payload, subscription_id);
            event_.mark_subscribe_response(!subscribed, subscription_id, now);
            if (subscribed &&
                pending_.event_command == EventCommandType::Quiesce) {
                Log::logf(CAT_RPC, LOG_INFO,
                          "AS11 event subscription quiesced\n");
            } else if (subscribed) {
                Log::logf(CAT_RPC, LOG_INFO,
                          "subscribed to events id=%lu\n",
                          static_cast<unsigned long>(subscription_id));
            } else {
                Log::logf(CAT_RPC, LOG_WARN,
                          "event subscription rejected\n");
            }
        }
    } else if (pending_.event_command != EventCommandType::None) {
        event_.mark_subscribe_response(true, 0, now);
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
                                      is_error, payload, now);
        if (is_error) stats_.stream_command_errors++;
    }
    const bool settings_refresh_succeeded =
        settings_updated && settings_snapshot_complete && !is_error;
    if (settings_refresh_succeeded && pending_.settings_refresh) {
        push_event(RpcEventKind::InternalSettingsStateUpdated, "",
                   pending_.source, pending_.id);
    }
    if (pending_.settings_refresh) {
        finish_as11_settings_refresh();
        if (!settings_refresh_succeeded) {
            schedule_as11_settings_refresh_retry(RpcSource::Scheduler, now);
        }
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

bool RpcArbiter::enqueue_as11_settings_refresh(RpcSource source) {
    QueuedRequest request;
    request.method = "Get";
    request.params_json = as11_settings_get_params_json();
    request.source = source;
    request.settings_refresh = true;
    if (!enqueue_request(request)) return false;

    settings_refresh_pending_count_++;
    return true;
}

void RpcArbiter::schedule_as11_settings_refresh_retry(RpcSource source,
                                                      uint32_t now) {
    if (!settings_refresh_retry_pending_ ||
        (scheduler_source(settings_refresh_retry_source_) &&
         !scheduler_source(source))) {
        settings_refresh_retry_source_ = source;
    }
    settings_refresh_retry_pending_ = true;

    uint32_t due = now + AS11_SETTINGS_REFRESH_RETRY_MS;
    if (scheduler_source(settings_refresh_retry_source_) &&
        background_backoff_active(now)) {
        due = background_backoff_until_ms_;
    }
    next_settings_refresh_retry_ms_ = due;
}

bool RpcArbiter::request_as11_settings_refresh(RpcSource source) {
    if (enqueue_as11_settings_refresh(source)) {
        settings_refresh_retry_pending_ = false;
        next_settings_refresh_retry_ms_ = 0;
        return true;
    }

    schedule_as11_settings_refresh_retry(source, millis());
    return false;
}

void RpcArbiter::poll_as11_settings_refresh(uint32_t now) {
    if (as11_settings_.expire_pending(
            now, AS11_SETTINGS_READBACK_TIMEOUT_MS)) {
        push_event(RpcEventKind::InternalSettingsStateUpdated, "",
                   RpcSource::Internal);
    }

    if (!settings_refresh_retry_pending_ || esp_ota_quiesce_requested_) return;
    if (static_cast<int32_t>(now - next_settings_refresh_retry_ms_) < 0) return;

    const RpcSource source = settings_refresh_retry_source_;
    if (enqueue_as11_settings_refresh(source)) {
        settings_refresh_retry_pending_ = false;
        next_settings_refresh_retry_ms_ = 0;
        return;
    }

    schedule_as11_settings_refresh_retry(source, now);
}

void RpcArbiter::finish_as11_settings_refresh() {
    if (settings_refresh_pending_count_) settings_refresh_pending_count_--;
}

bool RpcArbiter::handle_event_notification(const char *payload,
                                           size_t payload_len) {
    const uint32_t now = millis();
    As11EventFrame event_frame;
    const EventPublishResult event_result =
        event_.publish_notification(payload, payload_len, now, event_frame);
    if (!event_result.accepted) return false;
    push_event(RpcEventKind::InternalDeviceStateUpdated, "",
               RpcSource::Scheduler);

    const As11TherapyState before_state = as11_state_.therapy_state();
    const bool activity_updated =
        as11_state_.apply_activity_event_frame(event_frame, now);
    if (event_result.settings_history_change) {
        push_event(RpcEventKind::InternalSettingsStateInvalidated, "",
                   RpcSource::Scheduler);
        request_as11_settings_refresh(RpcSource::Scheduler);
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

void RpcArbiter::handle_stream_notification(const char *payload,
                                            size_t payload_len) {
    stats_.stream_notifications++;
    StreamPublishResult result =
        stream_.publish_stream_data(payload, payload_len, millis());
    stats_.stream_fanout_drops += result.drops;
    if (result.parse_error) stats_.stream_parse_errors++;
    if (result.pool_exhausted) stats_.stream_pool_exhaustions++;
    if (result.values_truncated) stats_.stream_truncated_frames++;
}

void RpcArbiter::enqueue_deferred_payload(DeferredPayload::Kind kind,
                                          const char *payload,
                                          size_t payload_len) {
    DeferredPayload deferred;
    if (!deferred.copy_from(kind, payload, payload_len)) {
        stats_.deferred_payload_alloc_failures++;
        stats_.deferred_payload_drops++;
        return;
    }
    if (!deferred_payloads_.push(std::move(deferred))) {
        stats_.deferred_payload_drops++;
        return;
    }
    stats_.deferred_payloads++;
}

void RpcArbiter::handle_frame(const RawCanFrame &frame) {
    if (frame.extended || frame.remote) return;
    const uint32_t now = millis();

    if (frame.id == AC_CAN_RX_ID) {
        DatagramFeedResult result = rpc_rx_.feed(frame.data, frame.len, now);
        if (result.status == DatagramStatus::Complete) {
            enqueue_deferred_payload(DeferredPayload::Kind::Rpc,
                                     result.payload_data,
                                     result.payload_len);
            rpc_rx_.reset();
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
            enqueue_deferred_payload(DeferredPayload::Kind::DebugLog,
                                     result.payload_data,
                                     result.payload_len);
            log_rx_.reset();
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
        deferred_payloads_.clear();
        cancel_all_requests("device_boot");
        as11_state_.reset();
        as11_settings_.clear();
        event_.mark_reattach(now);
        as11_healthcheck_initialized_ = true;
        schedule_as11_identity_refresh(now,
                                       AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_status_refresh(now, AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_motor_refresh(now, AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        schedule_as11_timezone_refresh(now,
                                       AC_AS11_INITIAL_STATUS_POLL_DELAY_MS);
        next_as11_clock_poll_ms_ = now + AC_AS11_INITIAL_STATUS_POLL_DELAY_MS +
            AC_RPC_DEFAULT_TIMEOUT_MS;
        if (stream_.desired_active()) stream_.mark_reattach(now);
        push_event(RpcEventKind::BootNotification, last_boot_notification_);
    }
}

void RpcArbiter::handle_rpc_payload(const char *payload, size_t payload_len) {
    stats_.rpc_datagrams++;
    auto make_payload_string = [&]() {
        return payload && payload_len ? std::string(payload, payload_len)
                                      : std::string();
    };

    RpcEnvelope envelope;
    (void)inspect_rpc_envelope(payload, payload_len, envelope);

    switch (envelope.kind) {
        case RpcPayloadKind::Notification: {
            stats_.rpc_notifications++;
            const bool stream_data = envelope.method_is("StreamData");
            const bool spool_fragment = envelope.method_is("SpoolFragment");
            const bool event_notification =
                envelope.method_is("EventNotification");
            if (stream_data) {
                handle_stream_notification(payload, payload_len);
            }
            if (event_notification) {
                handle_event_notification(payload, payload_len);
            }
            if (!stream_data && !spool_fragment) {
                Log::log_payload(CAT_RPC, LOG_DEBUG, "[NOTIFY] ",
                                 payload, payload_len);
            }
            RpcPayloadRef payload_ref;
            auto ref_payload = [&]() {
                if (!payload_ref) {
                    payload_ref = make_rpc_payload_ref(make_payload_string());
                }
                return payload_ref;
            };
            if (spool_fragment) {
                push_source_event(RpcSource::Report,
                                  RpcEventKind::RpcNotification,
                                  ref_payload(), RpcSource::Report);
            }
            if (raw_rpc_events_enabled_) {
                push_event(RpcEventKind::RpcNotification, ref_payload());
            } else if (!stream_data && !event_notification && !spool_fragment) {
                push_event(RpcEventKind::RpcNotification, ref_payload());
            }
            break;
        }
        case RpcPayloadKind::Response: {
            stats_.rpc_responses++;
            std::string owned_payload = make_payload_string();
            RpcPayloadRef payload_ref;
            auto ref_payload = [&]() {
                if (!payload_ref) {
                    payload_ref =
                        make_rpc_payload_ref(std::move(owned_payload));
                }
                return payload_ref;
            };
            const uint32_t response_id = envelope.id;
            const bool has_response_id = true;
            if (pending_.active && has_response_id &&
                response_id == pending_.id) {
                const uint32_t matched_id = pending_.id;
                const std::string matched_method = pending_.method;
                const RpcSource response_source = pending_.source;
                const bool settings_refresh = pending_.settings_refresh;
                if (response_source != RpcSource::Console) {
                    char prefix[112];
                    snprintf(prefix, sizeof(prefix),
                             "[RESPONSE id=%lu method=%s source=%s] ",
                             static_cast<unsigned long>(matched_id),
                             matched_method.c_str(),
                             source_name(response_source));
                    Log::log_payload(CAT_RPC, LOG_DEBUG, prefix,
                                     owned_payload);
                }
                handle_matched_response(owned_payload);
                note_request_success(response_source, millis());
                pending_ = {};
                if (source_event_route(response_source)) {
                    push_source_event(response_source,
                                      RpcEventKind::RpcResponse,
                                      ref_payload(), response_source,
                                      matched_id);
                    break;
                }
                if (settings_refresh) break;
                if (!emit_matched_response(response_source)) break;
                push_event(RpcEventKind::RpcResponse, ref_payload(),
                           response_source, matched_id);
                break;
            }
            RawPassthroughRequest passthrough_request;
            if (has_response_id &&
                match_raw_passthrough(response_id, passthrough_request,
                                      millis())) {
                const RpcSource passthrough_source =
                    passthrough_request.source;
                note_raw_stream_response(
                    passthrough_request.stream_command,
                    json_member_present(owned_payload, "error"));
                if (Log::get_cat_level(CAT_RPC) >= LOG_DEBUG) {
                    char prefix[112];
                    snprintf(prefix, sizeof(prefix),
                             "[RESPONSE id=%lu source=%s] ",
                             static_cast<unsigned long>(response_id),
                             source_name(passthrough_source));
                    Log::log_payload(CAT_RPC, LOG_DEBUG, prefix,
                                     owned_payload);
                }
                push_event(RpcEventKind::RpcResponse, ref_payload(),
                           passthrough_source, response_id);
                break;
            }
            if (pending_.active) {
                stats_.rpc_unmatched++;
                Log::logf(CAT_RPC, LOG_DEBUG,
                          "response did not match pending id=%lu\n",
                          static_cast<unsigned long>(pending_.id));
            }
            Log::log_payload(CAT_RPC, LOG_DEBUG, "[RESPONSE_UNMATCHED] ",
                             owned_payload);
            push_event(RpcEventKind::RpcResponse, ref_payload());
            break;
        }
        case RpcPayloadKind::Unknown: {
            stats_.rpc_unmatched++;
            std::string owned_payload = make_payload_string();
            Log::log_payload(CAT_RPC, LOG_DEBUG, "[UNMATCHED] ",
                             owned_payload);
            RpcPayloadRef payload_ref =
                make_rpc_payload_ref(std::move(owned_payload));
            push_event(RpcEventKind::RpcUnmatched, payload_ref);
            break;
        }
    }
}

void RpcArbiter::handle_debug_payload(const char *payload, size_t payload_len) {
    stats_.log_datagrams++;
    Log::log_payload(CAT_RPC, LOG_DEBUG, "[AS11] ", payload, payload_len);
    if (raw_rpc_events_enabled_) {
        push_event(RpcEventKind::DebugLog,
                   make_rpc_payload_ref(
                       payload && payload_len
                           ? std::string(payload, payload_len)
                           : std::string()));
    }
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
        case RpcSource::Report: return "report";
        case RpcSource::EdfRecorder: return "edf_recorder";
        default: return "?";
    }
}

uint8_t RpcArbiter::source_id(RpcSource source) const {
    return static_cast<uint8_t>(source);
}

}  // namespace aircannect
