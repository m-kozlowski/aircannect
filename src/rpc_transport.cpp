#include "rpc_transport.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "as11_rpc.h"
#include "debug_log.h"

namespace aircannect {
namespace {

bool emit_matched_response(RpcSource source) {
    return source == RpcSource::Console ||
           source == RpcSource::Tcp ||
           source == RpcSource::HttpApi;
}

bool interactive_source(RpcSource source) {
    return source == RpcSource::Console || source == RpcSource::Tcp ||
           source == RpcSource::HttpApi;
}

const char *completion_cause_name(RpcCompletionCause cause) {
    switch (cause) {
        case RpcCompletionCause::Response: return "response";
        case RpcCompletionCause::Timeout: return "timeout";
        case RpcCompletionCause::Cancelled: return "cancelled";
        case RpcCompletionCause::DispatchFailure: return "dispatch_failure";
    }
    return "unknown";
}

bool current_epoch_ms(int64_t &epoch_ms) {
    struct timeval tv = {};
    if (gettimeofday(&tv, nullptr) != 0) return false;
    if (tv.tv_sec < 1609459200) return false;
    epoch_ms = static_cast<int64_t>(tv.tv_sec) * 1000 +
               static_cast<int64_t>(tv.tv_usec / 1000);
    return true;
}

}  // namespace

RpcTransport::RpcTransport(RpcApplicationLink &link) : link_(link) {
    stats_started_ms_ = millis();
}

bool RpcTransport::reserve_reassembly_buffers() {
    return link_.begin();
}

void RpcTransport::poll() {
    const uint32_t now = millis();
    link_.poll(now);
    note_link_rx_pressure(now);

    process_link_events(AC_RPC_PAYLOAD_DRAIN_BUDGET);
    process_deferred_payloads(AC_RPC_PAYLOAD_DRAIN_BUDGET);

    check_pending_timeout();
    dispatch_next_request();
}

void RpcTransport::process_deferred_payloads(size_t budget) {
    for (size_t i = 0; i < budget; ++i) {
        DeferredPayload payload;
        if (!deferred_payloads_.pop(payload)) return;

        const RpcPayloadRef payload_ref = std::move(payload.payload);
        if (payload.kind == DeferredPayload::Kind::Rpc) {
            handle_rpc_payload(payload_ref);
        } else {
            handle_debug_payload(payload_ref);
        }
    }
}

void RpcTransport::process_link_events(size_t budget) {
    for (size_t i = 0; i < budget; ++i) {
        RpcLinkEvent event;
        if (!link_.take_event(event)) return;

        switch (event.kind) {
            case RpcLinkEventKind::Payload:
                enqueue_deferred_payload(DeferredPayload::Kind::Rpc,
                                         event.payload);
                break;
            case RpcLinkEventKind::FramingError:
                stats_.rpc_framing_errors++;
                publish_framing_error(link_.name(), event.detail);
                break;
            case RpcLinkEventKind::Disconnected:
                invalidate_link_state(event.detail.empty()
                                          ? "link_disconnected"
                                          : event.detail.c_str());
                break;
            case RpcLinkEventKind::TransportChanged:
                invalidate_link_state(event.detail.empty()
                                          ? "transport_changed"
                                          : event.detail.c_str());
                break;
        }
    }
}

bool RpcTransport::submit_raw_payload(const std::string &payload, RpcSource source) {
    if (as11_unavailable_) {
        if (interactive_source(source)) {
            push_text_event(RpcEventKind::Info,
                            "AS11 unavailable; RPC request rejected",
                            sizeof("AS11 unavailable; RPC request rejected") - 1);
        }
        return false;
    }

    if (quiesce_mode_) {
        push_text_event(
            RpcEventKind::Info,
            "raw RPC rejected while transport quiesce is active",
            sizeof("raw RPC rejected while transport quiesce is active") - 1);
        return false;
    }
    if (Log::get_cat_level(CAT_RPC) >= LOG_DEBUG) {
        char prefix[80];
        snprintf(prefix, sizeof(prefix), "[RAW request source=%s] ",
                 source_name(source));
        Log::log_payload(CAT_RPC, LOG_DEBUG, prefix, payload);
    }
    const RpcLinkSendResult send_result = send_payload(payload);
    if (send_result != RpcLinkSendResult::Accepted) {
        const char *message = "RPC link rejected payload";
        if (send_result == RpcLinkSendResult::Busy) {
            message = "RPC link busy; payload rejected";
        } else if (send_result == RpcLinkSendResult::Unavailable) {
            message = "RPC link unavailable; payload rejected";
        }
        if (interactive_source(source)) {
            push_text_event(RpcEventKind::Info, message, strlen(message));
        }
        return false;
    }

    uint32_t id = 0;
    if ((source == RpcSource::Console || source == RpcSource::Tcp) &&
        json_extract_id(payload, id)) {
        remember_raw_passthrough(id, source, millis());
    }
    return true;
}

RpcLinkSendResult RpcTransport::send_payload(const std::string &payload) {
    return link_.send(RpcPayloadView(payload.data(), payload.size()));
}

bool RpcTransport::send_request(const std::string &method,
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

OperationSubmission RpcTransport::request(const RpcRequestCommand &command) {
    if (!command.valid()) return OperationSubmission::rejected();
    if (request_completion_reservations_ >= request_completions_.capacity()) {
        return OperationSubmission::busy();
    }

    QueuedRequest request;
    request.method = command.method;
    request.params_json = command.params_json;
    request.source = command.source;
    request.timeout_ms = command.timeout_ms;
    request.generation = command.generation;
    request.admission = command.admission;
    request.dispatch_window = command.dispatch_window;
    if (!request_allowed_while_unavailable(request)) {
        return OperationSubmission::rejected();
    }
    if (!enqueue_request(request)) return OperationSubmission::busy();

    request_completion_reservations_++;
    return OperationSubmission::accepted({request.id, request.generation});
}

bool RpcTransport::cancel(OperationTicket ticket) {
    if (!ticket.valid()) return false;

    if (pending_.active && pending_.id == ticket.id &&
        pending_.generation == ticket.generation) {
        cancel_pending_request("client_cancelled");
        return true;
    }

    if (dispatch_retry_active_ && dispatch_retry_.id == ticket.id &&
        dispatch_retry_.generation == ticket.generation) {
        cancel_queued_request(dispatch_retry_, "client_cancelled");
        dispatch_retry_ = {};
        dispatch_retry_active_ = false;
        dispatch_retry_deadline_ms_ = 0;
        next_dispatch_retry_ms_ = 0;
        return true;
    }

    bool cancelled = false;
    QueuedRequest request;
    FixedQueue<QueuedRequest, AC_RPC_REQUEST_QUEUE_DEPTH> keep;
    while (requests_.pop(request)) {
        if (!cancelled && request.id == ticket.id &&
            request.generation == ticket.generation) {
            cancel_queued_request(request, "client_cancelled");
            cancelled = true;
        } else {
            keep.push(std::move(request));
        }
    }
    while (keep.pop(request)) requests_.push(std::move(request));
    return cancelled;
}

bool RpcTransport::take_completion(OperationTicket ticket,
                                 RpcRequestCompletion &completion) {
    if (!ticket.valid()) return false;

    bool found = false;
    RpcRequestCompletion current;
    FixedQueue<RpcRequestCompletion, AC_RPC_REQUEST_QUEUE_DEPTH> keep;
    while (request_completions_.pop(current)) {
        if (!found && current.ticket == ticket) {
            completion = std::move(current);
            found = true;
        } else {
            keep.push(std::move(current));
        }
    }
    while (keep.pop(current)) request_completions_.push(std::move(current));
    if (found && request_completion_reservations_) {
        request_completion_reservations_--;
    }
    return found;
}

bool RpcTransport::enqueue_request(QueuedRequest &request) {
    if (!request_allowed_while_unavailable(request)) {
        if (interactive_source(request.source)) {
            push_text_event(RpcEventKind::Info,
                            "AS11 unavailable; RPC request rejected",
                            sizeof("AS11 unavailable; RPC request rejected") - 1);
        }
        return false;
    }

    request.id = ++next_rpc_id_;
    if (!requests_.push(request)) {
        stats_.request_queue_drops++;
        push_text_event(RpcEventKind::Info,
                        "RPC request queue full",
                        sizeof("RPC request queue full") - 1);
        return false;
    }

    return true;
}

bool RpcTransport::next_event(RpcEvent &event) {
    return events_.pop(event);
}

bool RpcTransport::background_backpressure_active() const {
    if (background_rx_pressure_active(millis())) return true;

    if (link_.status().rx_pressure) return true;
    if (deferred_payloads_.count() >=
        AC_RPC_PAYLOAD_BACKPRESSURE_WATERMARK) {
        return true;
    }
    return false;
}

void RpcTransport::set_as11_unavailable(bool unavailable) {
    if (unavailable == as11_unavailable_) return;

    as11_unavailable_ = unavailable;
    if (unavailable) cancel_requests_while_unavailable();
}

void RpcTransport::set_controlled_disconnect(bool requested) {
    link_.set_controlled_disconnect(requested);
}

bool RpcTransport::controlled_disconnect_complete() const {
    return link_.controlled_disconnect_complete();
}

void RpcTransport::set_raw_rpc_forwarding_enabled(bool enabled) {
    raw_rpc_forwarding_enabled_ = enabled;
}

void RpcTransport::set_event_notification_observer(
    RpcNotificationObserver observer,
    void *context) {
    event_notification_observer_ = observer;
    event_notification_context_ = observer ? context : nullptr;
}

void RpcTransport::set_stream_notification_observer(
    RpcNotificationObserver observer,
    void *context) {
    stream_notification_observer_ = observer;
    stream_notification_context_ = observer ? context : nullptr;
}

void RpcTransport::set_spool_notification_observer(
    RpcRetainedNotificationObserver observer,
    void *context) {
    spool_notification_observer_ = observer;
    spool_notification_context_ = observer ? context : nullptr;
}

void RpcTransport::accept_debug_payload(const RpcPayloadRef &payload) {
    enqueue_deferred_payload(DeferredPayload::Kind::DebugLog, payload);
}

void RpcTransport::accept_debug_framing_error(const char *detail) {
    stats_.log_framing_errors++;
    const std::string error = detail ? detail : "framing_error";
    Log::logf(CAT_CAN, LOG_WARN, "[DEBUG][FRAMING] %s\n",
              error.c_str());
    publish_framing_error("can-log", error);
}

void RpcTransport::accept_boot_notification(const char *detail) {
    last_boot_notification_ = detail ? detail : "device_boot";
    last_boot_notification_ms_ = millis();
    boot_notifications_seen_++;

    deferred_payloads_.clear();
    cancel_all_requests("device_boot");
    note_transport_reset();
    push_text_event(RpcEventKind::BootNotification,
                    last_boot_notification_.data(),
                    last_boot_notification_.size());
}

void RpcTransport::accept_link_reset(const char *reason) {
    link_.reset();
    invalidate_link_state(reason ? reason : "link_reset");
}

void RpcTransport::invalidate_link_state(const char *reason) {
    deferred_payloads_.clear();
    cancel_all_requests(reason);
    note_transport_reset();
}

void RpcTransport::reset_stats() {
    stats_ = {};
    observed_rx_pressure_events_ = 0;
    stats_started_ms_ = millis();
}

RpcTransportStatus RpcTransport::runtime_status() const {
    const uint32_t now = millis();
    RpcTransportStatus out;
    out.stats_elapsed_ms = std::max<uint32_t>(1, now - stats_started_ms_);
    out.request_queue_depth = requests_.count();
    out.payload_queue_depth = deferred_payloads_.count();
    out.pending_request_id = pending_.active ? pending_.id : 0;
    out.dispatch_retry_id = dispatch_retry_active_ ? dispatch_retry_.id : 0;
    const uint32_t rx_pressure_backoff_ms = background_rx_pressure_active(now)
        ? background_rx_pressure_until_ms_ - now
        : 0;
    out.rx_pressure_backoff_ms = rx_pressure_backoff_ms;
    out.boot_notifications = boot_notifications_seen_;
    if (!last_boot_notification_.empty()) {
        out.last_boot_notification_age_ms = now - last_boot_notification_ms_;
        out.last_boot_notification = last_boot_notification_;
    }
    return out;
}

void RpcTransport::set_quiesce_mode(bool requested) {
    if (requested == quiesce_mode_) return;

    quiesce_mode_ = requested;
    if (requested) {
        cancel_all_requests("rpc_quiesce");
    }
}

bool RpcTransport::send_quiesce_request(
    const std::string &method,
    const std::string &params_json) {
    if (!quiesce_mode_ || method.empty() || !quiesce_idle()) return false;

    const uint32_t id = ++next_rpc_id_;
    const std::string payload = build_rpc_request(method, params_json, id);
    if (send_payload(payload) !=
        RpcLinkSendResult::Accepted) {
        return false;
    }

    Log::logf(CAT_RPC, LOG_DEBUG,
              "quiesce request dispatched id=%lu method=%s\n",
              static_cast<unsigned long>(id), method.c_str());
    return true;
}

bool RpcTransport::quiesce_idle() const {
    const RpcApplicationLinkStatus link_status = link_.status();
    return !pending_.active && !dispatch_retry_active_ && requests_.empty() &&
           deferred_payloads_.empty() && link_status.tx_idle;
}

RpcQuiesceStatus RpcTransport::quiesce_status() const {
    RpcQuiesceStatus out;
    out.idle = quiesce_idle();
    out.pending_request = pending_.active;
    out.dispatch_retry = dispatch_retry_active_;
    out.request_queue_depth = requests_.count();
    out.payload_queue_depth = deferred_payloads_.count();
    out.tx_queue_depth = link_.status().tx_queue_depth;
    return out;
}

bool RpcTransport::background_rx_pressure_active(uint32_t now) const {
    return background_rx_pressure_until_ms_ &&
           static_cast<int32_t>(background_rx_pressure_until_ms_ - now) > 0;
}

void RpcTransport::note_link_rx_pressure(uint32_t now) {
    const uint32_t events = link_.status().rx_pressure_events;
    if (events == observed_rx_pressure_events_) return;
    observed_rx_pressure_events_ = events;

    const uint32_t until =
        now + AC_RPC_BACKGROUND_RX_PRESSURE_BACKOFF_MS;
    if (!background_rx_pressure_until_ms_ ||
        static_cast<int32_t>(until - background_rx_pressure_until_ms_) > 0) {
        background_rx_pressure_until_ms_ = until;
    }
}

bool RpcTransport::request_allowed_during_quiesce(
    const QueuedRequest &request) const {
    if (!quiesce_mode_) return true;
    return request.admission == RpcRequestAdmission::QuiesceControl;
}

bool RpcTransport::request_allowed_while_unavailable(
    const QueuedRequest &request) const {
    if (!as11_unavailable_) return true;
    return request.admission == RpcRequestAdmission::PresenceProbe ||
           request.admission == RpcRequestAdmission::QuiesceControl;
}

void RpcTransport::cancel_requests_while_unavailable() {
    if (pending_.active &&
        pending_.admission == RpcRequestAdmission::Normal) {
        cancel_pending_request("as11_unavailable");
    }

    if (dispatch_retry_active_ &&
        dispatch_retry_.admission == RpcRequestAdmission::Normal) {
        cancel_queued_request(dispatch_retry_, "as11_unavailable");
        dispatch_retry_ = {};
        dispatch_retry_active_ = false;
        dispatch_retry_deadline_ms_ = 0;
        next_dispatch_retry_ms_ = 0;
    }

    QueuedRequest request;
    FixedQueue<QueuedRequest, AC_RPC_REQUEST_QUEUE_DEPTH> keep;
    while (requests_.pop(request)) {
        if (request.admission == RpcRequestAdmission::Normal) {
            cancel_queued_request(request, "as11_unavailable");
        } else {
            keep.push(std::move(request));
        }
    }
    while (keep.pop(request)) requests_.push(std::move(request));
}

void RpcTransport::push_event(RpcEventKind kind,
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

void RpcTransport::push_text_event(RpcEventKind kind,
                                   const char *payload,
                                   size_t payload_len,
                                   RpcSource source,
                                   uint32_t id) {
    RpcPayloadRef owned = copy_rpc_payload(payload, payload_len);
    if (payload_len != 0 && !owned) {
        stats_.event_drops++;
        return;
    }
    push_event(kind, std::move(owned), source, id);
}

void RpcTransport::publish_framing_error(const char *channel,
                                         const std::string &error) {
    const char *name = channel ? channel : "?";
    const std::string event_payload = std::string("[") + name + "] " + error;
    push_text_event(RpcEventKind::FramingError,
                    event_payload.data(), event_payload.size());
}

void RpcTransport::cancel_pending_request(const char *reason) {
    if (!pending_.active) return;

    stats_.request_cancellations++;
    const bool addressed_request = pending_.generation != 0;
    if (addressed_request) {
        remember_retired_addressed_request(
            pending_, RpcCompletionCause::Cancelled, reason);
    }
    if (!addressed_request && pending_.source != RpcSource::Scheduler) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(pending_.id),
                 pending_.method.c_str(),
                 source_name(pending_.source),
                 reason ? reason : "unknown");
        push_text_event(RpcEventKind::Info, buf, strlen(buf));
    }
    complete_request(pending_.id, pending_.generation,
                     OperationOutcome::cancelled(),
                     RpcCompletionCause::Cancelled, RpcPayloadRef(),
                     reason ? reason : "cancelled", false,
                     request_completions_);
    pending_ = {};
}

void RpcTransport::remember_retired_addressed_request(
    const PendingRequest &request,
    RpcCompletionCause cause,
    const char *reason) {
    if (!request.active || !request.generation || !request.id) return;

    RetiredAddressedRequest &retired =
        retired_addressed_requests_[next_retired_addressed_request_];
    retired.id = request.id;
    retired.retired_ms = millis();
    retired.cause = cause;
    next_retired_addressed_request_ =
        (next_retired_addressed_request_ + 1) %
        RETIRED_ADDRESSED_REQUEST_MAX;

    Log::logf(CAT_RPC, LOG_DEBUG,
              "retired request id=%lu method=%s source=%s cause=%s "
              "reason=%s\n",
              static_cast<unsigned long>(request.id),
              request.method.c_str(),
              source_name(request.source),
              completion_cause_name(cause),
              reason ? reason : "--");
}

bool RpcTransport::consume_retired_addressed_response(uint32_t id,
                                                       uint32_t now_ms) {
    if (!id) return false;

    for (RetiredAddressedRequest &retired : retired_addressed_requests_) {
        if (!retired.id) continue;

        const uint32_t age_ms = now_ms - retired.retired_ms;
        if (age_ms >= RETIRED_ADDRESSED_REQUEST_TTL_MS) {
            retired = {};
            continue;
        }
        if (retired.id != id) continue;

        stats_.late_addressed_responses++;
        Log::logf(CAT_RPC, LOG_DEBUG,
                  "late response consumed id=%lu cause=%s age_ms=%lu\n",
                  static_cast<unsigned long>(id),
                  completion_cause_name(retired.cause),
                  static_cast<unsigned long>(age_ms));
        retired = {};
        return true;
    }
    return false;
}

void RpcTransport::cancel_queued_request(const QueuedRequest &request,
                                       const char *reason,
                                       RpcCompletionCause cause) {
    stats_.request_cancellations++;
    const bool addressed_request = request.generation != 0;
    if (!addressed_request && request.source != RpcSource::Scheduler) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "RPC request cancelled id=%lu method=%s source=%s reason=%s",
                 static_cast<unsigned long>(request.id),
                 request.method.c_str(),
                 source_name(request.source),
                 reason ? reason : "unknown");
        push_text_event(RpcEventKind::Info, buf, strlen(buf));
    }
    const OperationOutcome outcome =
        cause == RpcCompletionCause::Cancelled
            ? OperationOutcome::cancelled()
            : OperationOutcome::failed();
    complete_request(request.id, request.generation, outcome, cause,
                     RpcPayloadRef(),
                     reason ? reason : "cancelled", false,
                     request_completions_);
}

void RpcTransport::complete_request(uint32_t id,
                                    uint32_t generation,
                                    OperationOutcome outcome,
                                    RpcCompletionCause cause,
                                    const RpcPayloadRef &payload,
                                    const char *reason,
                                    bool response_error,
                                    RequestCompletionQueue &completions,
                                    int64_t dispatch_utc_ms,
                                    int64_t response_utc_ms,
                                    uint32_t dispatch_ms,
                                    uint32_t response_ms) {
    if (!generation) return;

    RpcRequestCompletion completion;
    completion.ticket = {id, generation};
    completion.outcome = outcome;
    completion.cause = cause;
    completion.payload = payload;
    completion.reason = reason ? reason : "";
    completion.response_error = response_error;
    completion.dispatch_utc_ms = dispatch_utc_ms;
    completion.response_utc_ms = response_utc_ms;
    completion.dispatch_ms = dispatch_ms;
    completion.response_ms = response_ms;
    if (!completions.push(std::move(completion))) {
        Log::logf(CAT_RPC, LOG_ERROR,
                  "RPC completion queue invariant violated id=%lu\n",
                  static_cast<unsigned long>(id));
    }
}

void RpcTransport::cancel_all_requests(const char *reason) {
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

void RpcTransport::expire_raw_passthrough(uint32_t now) {
    for (auto &request : raw_passthrough_) {
        if (!request.active) continue;
        if (static_cast<int32_t>(now - request.deadline_ms) >= 0) {
            request = {};
        }
    }
}

void RpcTransport::remember_raw_passthrough(uint32_t id,
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
    slot->deadline_ms = now + AC_RPC_RAW_PASSTHROUGH_TIMEOUT_MS;
}

bool RpcTransport::match_raw_passthrough(uint32_t id,
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

void RpcTransport::dispatch_next_request() {
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

    if (quiesce_mode_ && !request_allowed_during_quiesce(request)) {
        cancel_queued_request(request, "rpc_quiesce");
        if (from_retry) {
            dispatch_retry_ = {};
            dispatch_retry_active_ = false;
            dispatch_retry_deadline_ms_ = 0;
            next_dispatch_retry_ms_ = 0;
        }
        return;
    }

    if (!request_allowed_while_unavailable(request)) {
        cancel_queued_request(request, "as11_unavailable");
        if (from_retry) {
            dispatch_retry_ = {};
            dispatch_retry_active_ = false;
            dispatch_retry_deadline_ms_ = 0;
            next_dispatch_retry_ms_ = 0;
        }
        return;
    }

    if (request.dispatch_window.enabled) {
        if (static_cast<int32_t>(
                now - request.dispatch_window.not_before_ms) < 0) {
            dispatch_retry_ = request;
            dispatch_retry_active_ = true;
            dispatch_retry_deadline_ms_ =
                request.dispatch_window.deadline_ms;
            next_dispatch_retry_ms_ =
                request.dispatch_window.not_before_ms;
            return;
        }
        if (static_cast<int32_t>(
                now - request.dispatch_window.deadline_ms) > 0) {
            cancel_queued_request(request, "dispatch_window_expired",
                                  RpcCompletionCause::DispatchFailure);
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
    const RpcLinkSendResult send_result = send_payload(payload);
    if (send_result != RpcLinkSendResult::Accepted) {
        if (!dispatch_retry_active_) {
            dispatch_retry_ = request;
            dispatch_retry_active_ = true;
            dispatch_retry_deadline_ms_ = now + request.timeout_ms;
        }
        next_dispatch_retry_ms_ = now + AC_RPC_MIN_TX_INTERVAL_MS;
        stats_.request_dispatch_retries++;
        if (static_cast<int32_t>(now - dispatch_retry_deadline_ms_) >= 0) {
            const char *reason = send_result == RpcLinkSendResult::Unavailable
                ? "dispatch_link_unavailable"
                : "dispatch_link_busy";
            cancel_queued_request(dispatch_retry_, reason,
                                  RpcCompletionCause::DispatchFailure);
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
    pending_.admission = request.admission;
    pending_.generation = request.generation;
    pending_.dispatch_ms = millis();
    pending_.deadline_ms = pending_.dispatch_ms + request.timeout_ms;
    pending_.dispatch_utc_ms = 0;
    (void)current_epoch_ms(pending_.dispatch_utc_ms);
    last_integrated_tx_ms_ = pending_.dispatch_ms;
    stats_.dispatched_requests++;

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

void RpcTransport::check_pending_timeout() {
    if (!pending_.active) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - pending_.deadline_ms) < 0) return;

    char buf[128];
    snprintf(buf, sizeof(buf), "RPC request timeout id=%lu method=%s source=%s",
             static_cast<unsigned long>(pending_.id),
             pending_.method.c_str(),
             source_name(pending_.source));
    const bool addressed_request = pending_.generation != 0;
    if (!addressed_request && pending_.source != RpcSource::Scheduler) {
        push_text_event(RpcEventKind::Info, buf, strlen(buf));
    }
    stats_.request_timeouts++;
    if (addressed_request) {
        remember_retired_addressed_request(
            pending_, RpcCompletionCause::Timeout, "timeout");
    }
    complete_request(pending_.id, pending_.generation,
                     OperationOutcome::failed(), RpcCompletionCause::Timeout,
                     RpcPayloadRef(), "timeout", false, request_completions_);

    pending_ = {};
}

void RpcTransport::handle_event_notification(const RpcPayloadRef &payload) {
    if (!event_notification_observer_) return;

    event_notification_observer_(event_notification_context_,
                                 rpc_payload_view(payload), millis());
}

void RpcTransport::handle_stream_notification(const RpcPayloadRef &payload) {
    if (!stream_notification_observer_) return;

    stream_notification_observer_(stream_notification_context_,
                                  rpc_payload_view(payload), millis());
}

void RpcTransport::handle_spool_notification(const RpcPayloadRef &payload) {
    if (!spool_notification_observer_) return;
    spool_notification_observer_(spool_notification_context_, payload, millis());
}

void RpcTransport::note_transport_reset() {
    transport_generation_++;
    if (transport_generation_ == 0) transport_generation_++;
}

void RpcTransport::enqueue_deferred_payload(DeferredPayload::Kind kind,
                                          RpcPayloadView payload) {
    DeferredPayload deferred;
    deferred.kind = kind;
    deferred.payload = copy_rpc_payload(payload.data(), payload.size());
    if (!payload.empty() && !deferred.payload) {
        stats_.deferred_payload_alloc_failures++;
        stats_.deferred_payload_drops++;
        return;
    }
    if (!deferred_payloads_.push(std::move(deferred))) {
        stats_.deferred_payload_drops++;
        return;
    }
}

void RpcTransport::enqueue_deferred_payload(
    DeferredPayload::Kind kind,
    const RpcPayloadRef &payload) {
    DeferredPayload deferred;
    deferred.kind = kind;
    deferred.payload = payload;
    if (!deferred_payloads_.push(std::move(deferred))) {
        stats_.deferred_payload_drops++;
    }
}

void RpcTransport::handle_rpc_payload(const RpcPayloadRef &payload) {
    stats_.rpc_datagrams++;
    const RpcPayloadView view = rpc_payload_view(payload);

    RpcEnvelope envelope;
    (void)inspect_rpc_envelope(view.data(), view.size(), envelope);

    switch (envelope.kind) {
        case RpcPayloadKind::Notification: {
            stats_.rpc_notifications++;
            const bool stream_data = envelope.method_is("StreamData");
            const bool spool_fragment = envelope.method_is("SpoolFragment");
            const bool event_notification =
                envelope.method_is("EventNotification");
            if (stream_data) {
                handle_stream_notification(payload);
            }
            if (event_notification) {
                handle_event_notification(payload);
            }
            if (spool_fragment) {
                handle_spool_notification(payload);
            }
            if (!stream_data && !spool_fragment) {
                Log::log_payload(CAT_RPC, LOG_DEBUG, "[NOTIFY] ",
                                 view.data(), view.size());
            }
            if (raw_rpc_forwarding_enabled_) {
                push_event(RpcEventKind::RpcNotification, payload);
            } else if (!stream_data && !event_notification && !spool_fragment) {
                push_event(RpcEventKind::RpcNotification, payload);
            }
            break;
        }
        case RpcPayloadKind::Response: {
            stats_.rpc_responses++;
            const uint32_t response_id = envelope.id;
            const bool has_response_id = true;
            if (pending_.active && has_response_id &&
                response_id == pending_.id) {
                const uint32_t matched_id = pending_.id;
                const std::string matched_method = pending_.method;
                const RpcSource response_source = pending_.source;
                const bool addressed_request = pending_.generation != 0;
                if (response_source != RpcSource::Console) {
                    char prefix[112];
                    snprintf(prefix, sizeof(prefix),
                             "[RESPONSE id=%lu method=%s source=%s] ",
                             static_cast<unsigned long>(matched_id),
                             matched_method.c_str(),
                             source_name(response_source));
                    Log::log_payload(CAT_RPC, LOG_DEBUG, prefix,
                                     view.data(), view.size());
                }
                const bool response_error =
                    json_member_present(view.data(), view.size(), "error");
                const uint32_t response_ms = millis();
                int64_t response_epoch_ms = 0;
                (void)current_epoch_ms(response_epoch_ms);
                complete_request(
                    pending_.id, pending_.generation,
                    response_error ? OperationOutcome::failed()
                                   : OperationOutcome::succeeded(),
                    RpcCompletionCause::Response, payload, "",
                    response_error, request_completions_,
                    pending_.dispatch_utc_ms, response_epoch_ms,
                    pending_.dispatch_ms, response_ms);
                pending_ = {};
                if (addressed_request) break;
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
                             "[RESPONSE id=%lu source=%s] ",
                             static_cast<unsigned long>(response_id),
                             source_name(passthrough_source));
                    Log::log_payload(CAT_RPC, LOG_DEBUG, prefix,
                                     view.data(), view.size());
                }
                push_event(RpcEventKind::RpcResponse, payload,
                           passthrough_source, response_id);
                break;
            }
            if (has_response_id &&
                consume_retired_addressed_response(response_id, millis())) {
                break;
            }
            if (pending_.active) {
                stats_.rpc_unmatched++;
                Log::logf(CAT_RPC, LOG_DEBUG,
                          "response did not match pending id=%lu\n",
                          static_cast<unsigned long>(pending_.id));
            }
            Log::log_payload(CAT_RPC, LOG_DEBUG, "[RESPONSE_UNMATCHED] ",
                             view.data(), view.size());
            push_event(RpcEventKind::RpcResponse, payload);
            break;
        }
        case RpcPayloadKind::Unknown: {
            stats_.rpc_unmatched++;
            Log::log_payload(CAT_RPC, LOG_DEBUG, "[UNMATCHED] ",
                             view.data(), view.size());
            push_event(RpcEventKind::RpcUnmatched, payload);
            break;
        }
    }
}

void RpcTransport::handle_debug_payload(const RpcPayloadRef &payload) {
    const RpcPayloadView view = rpc_payload_view(payload);
    Log::log_payload(CAT_RPC, LOG_DEBUG, "[AS11] ", view.data(), view.size());
    if (raw_rpc_forwarding_enabled_) {
        push_event(RpcEventKind::DebugLog, payload);
    }
}

const char *RpcTransport::source_name(RpcSource source) const {
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

}  // namespace aircannect
